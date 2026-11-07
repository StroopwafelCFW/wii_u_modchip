#!/usr/bin/env python3
"""
picoterm - serial terminal + dev helper for minute / stroopwafel via the de_Fuse Pico.

Run `picoterm.py term` in place of minicom. It owns the Pico's tty and listens on a
control socket, so the other subcommands can drive the console while you
watch the output in the terminal. If no terminal is running, the subcommands
open the port themselves.

  picoterm.py term                       interactive terminal (Ctrl-T h for help)
  picoterm.py reset                      reset the console (Pico pulses NRST)
  picoterm.py send "help"                type a line into minute's console
  picoterm.py plugin a.ipx b.ipx         reset -> minute -> upload plugins -> boot entry 3
  picoterm.py plugin --rm old.ipx        ...delete plugins (can be combined with uploads)
  picoterm.py plugins                    list sdmc:/wiiu/ios_plugins
  picoterm.py fw fw.img                  reset -> minute -> upload sdmc:/fw.img -> reload
  picoterm.py console                    reset and enter minute's interactive console
  picoterm.py flash-pico [file.uf2]      reboot the Pico into BOOTSEL and flash it

Needs the serial-only Pico firmware (pico_serial.uf2) for `reset`:
baud 1201 = reset the console, baud 1200 = Pico BOOTSEL.
"""

import argparse
import fcntl
import glob
import hashlib
import json
import os
import re
import select
import shutil
import socket
import struct
import subprocess
import sys
import termios
import threading
import time
import tty

SCRIPT_DIR = os.path.dirname(os.path.realpath(__file__))
PORT_GLOB = '/dev/serial/by-id/usb-Raspberry_Pi_Pico_*-if00'
DEFAULT_PORT = os.environ.get('PICOTERM_PORT')  # else autodetect via PORT_GLOB
SOCK_PATH = os.environ.get('PICOTERM_SOCK') or (
    os.path.join(os.environ['XDG_RUNTIME_DIR'], 'picoterm.sock') if os.environ.get('XDG_RUNTIME_DIR')
    else '/tmp/picoterm-%d.sock' % os.getuid())
DEFAULT_UF2 = os.path.join(SCRIPT_DIR, 'pico_defuse', 'build', 'pico_serial.uf2')

NORMAL_BAUD = 115200
RESET_BAUD = 1201
BOOTSEL_BAUD = 1200

MAGIC_UPLD = bytes([0x55, 0xAA] * 4 + [0x55, 0x50, 0x4C, 0x44])
MAGIC_SYNC = bytes([0x55, 0xAA] * 4)

# minute's interactive console redraws its prompt with this sequence
INTCON_PROMPT = r'\x1b\[0G> '
PLUGIN_DIR = 'sdmc:/wiiu/ios_plugins'
DEFAULT_BOOT_ENTRY = 3  # "Patch (sd) and boot IOS (slc)"

# legacy env vars of miniterm_defuse.py, used if nothing was registered
LEGACY_ENV = {
    'sdmc:/fw.img': 'MINUTE_MINUTE_FW_IMG',
    'sdmc:/ios.patch': 'MINUTE_MINUTE_IOS_PATCH',
    'sdmc:/ios.img': 'MINUTE_MINUTE_IOS_IMG',
    'sdmc:/wiiu/ios_plugins/wafel_core.ipx': 'MINUTE_MINUTE_IOS_PLG',
    'sdmc:/boot1_slccmpt.img': 'MINUTE_MINUTE_BOOT1_IMG',
}


class PicotermError(Exception):
    pass


# --------------------------------------------------------------------------
# tty helpers

# Linux termios2 (arbitrary baud rates); same numbers on x86_64 and arm64
TCGETS2 = 0x802C542A
TCSETS2 = 0x402C542B
BOTHER = 0o010000
CBAUD = 0o010017
IBSHIFT = 16


def set_baud(fd, baud):
    """Set an arbitrary baud rate; on a CDC-ACM device this sends SET_LINE_CODING."""
    buf = bytearray(44)
    fcntl.ioctl(fd, TCGETS2, buf)
    cflag = struct.unpack_from('I', buf, 8)[0]
    cflag &= ~(CBAUD | (CBAUD << IBSHIFT))
    cflag |= BOTHER | (BOTHER << IBSHIFT)
    struct.pack_into('I', buf, 8, cflag)
    struct.pack_into('II', buf, 36, baud, baud)
    fcntl.ioctl(fd, TCSETS2, buf)


def find_port(explicit=None):
    if explicit:
        return explicit if os.path.exists(explicit) else None
    ports = sorted(glob.glob(PORT_GLOB))
    if ports:
        return ports[0]
    return '/dev/ttyACM0' if os.path.exists('/dev/ttyACM0') else None


def port_users(path):
    """Other processes that have the tty open, as a list of "pid name" strings."""
    try:
        real = os.path.realpath(path)
    except OSError:
        return []
    users = []
    me = os.getpid()
    for fd_dir in glob.glob('/proc/[0-9]*/fd'):
        pid = int(fd_dir.split('/')[2])
        if pid == me:
            continue
        try:
            for fd in os.listdir(fd_dir):
                if os.readlink(os.path.join(fd_dir, fd)) == real:
                    with open('/proc/%d/comm' % pid) as f:
                        users.append('%d %s' % (pid, f.read().strip()))
                    break
        except OSError:
            continue
    return users


class Port:
    def __init__(self, explicit=None):
        self.explicit = explicit
        self.fd = None
        self.name = None

    def open(self):
        path = find_port(self.explicit)
        if not path:
            raise OSError('Pico serial port not found')
        fd = os.open(path, os.O_RDWR | os.O_NOCTTY)
        try:
            tty.setraw(fd)
            attrs = termios.tcgetattr(fd)
            attrs[2] |= termios.CLOCAL | termios.CREAD
            termios.tcsetattr(fd, termios.TCSANOW, attrs)
            set_baud(fd, NORMAL_BAUD)
            try:
                fcntl.ioctl(fd, termios.TIOCEXCL)  # keep minicom & co out
            except OSError:
                pass
        except Exception:
            os.close(fd)
            raise
        self.fd = fd
        self.name = path

    def close(self):
        if self.fd is not None:
            try:
                fcntl.ioctl(self.fd, termios.TIOCNXCL)
            except OSError:
                pass
            try:
                os.close(self.fd)
            except OSError:
                pass
        self.fd = None

    def write(self, data):
        if self.fd is None:
            raise OSError('port not open')
        view = memoryview(data)
        while view:
            n = os.write(self.fd, view)
            view = view[n:]

    def drain(self):
        if self.fd is not None:
            termios.tcdrain(self.fd)


# --------------------------------------------------------------------------
# Engine: owns the port, logs output, answers minute's upload requests

class Engine:
    LOG_MAX = 4 << 20

    def __init__(self, port_name=None, raw_out=None, msg_out=None, msg_eol='\n'):
        self.port = Port(port_name)
        self.raw_out = raw_out        # callable(bytes) for console output, or None
        self.msg_out = msg_out or sys.stderr
        self.msg_eol = msg_eol
        self.wlock = threading.RLock()
        self.cond = threading.Condition()
        self.log = bytearray()
        self.log_base = 0
        self.serve_map = {}
        self.tail = b''
        self.upld_line = None
        self.line = bytearray()
        self.pending_sha = None
        self.alive = True
        self.connected = threading.Event()
        self.last_error = None
        self.thread = None

    # -- messages
    def msg(self, text):
        eol = self.msg_eol
        self.msg_out.write(eol + '--- ' + text.replace('\n', eol) + eol)
        self.msg_out.flush()

    # -- lifecycle
    def start(self, wait=True):
        self.thread = threading.Thread(target=self._reader, daemon=True)
        self.thread.start()
        if wait and not self.connected.wait(5):
            raise PicotermError('cannot open the Pico serial port: %s' % self.last_error)

    def stop(self):
        self.alive = False
        self.port.close()

    def _reader(self):
        waiting_said = False
        while self.alive:
            if self.port.fd is None:
                try:
                    self.port.open()
                    self.msg('connected to %s' % self.port.name)
                    waiting_said = False
                    self.connected.set()
                except OSError as e:
                    self.last_error = e
                    if not waiting_said:
                        self.msg('waiting for Pico (%s)...' % e)
                        waiting_said = True
                    time.sleep(0.5)
                    continue
            try:
                r, _, _ = select.select([self.port.fd], [], [], 0.2)
                if not r:
                    continue
                data = os.read(self.port.fd, 4096)
                if not data:
                    raise OSError('EOF')
            except (OSError, ValueError, TypeError) as e:
                if not self.alive:
                    break
                self.connected.clear()
                self.port.close()
                self.msg('Pico disconnected (%s)' % e)
                continue
            self._feed(data)

    def _feed(self, data):
        if self.raw_out:
            self.raw_out(data)
        with self.cond:
            self.log.extend(data)
            if len(self.log) > self.LOG_MAX:
                drop = len(self.log) - self.LOG_MAX // 2
                del self.log[:drop]
                self.log_base += drop
            self.cond.notify_all()
        for b in data:
            self._scan(b)

    def _scan(self, b):
        self.tail = (self.tail + bytes([b]))[-len(MAGIC_UPLD):]
        if self.upld_line is not None:
            if b == 0x0A:
                path = bytes(self.upld_line).strip(b'\r\n\x00 ').decode('latin-1')
                self.upld_line = bytearray()
                if path:  # first line after the magic is just its own newline
                    self.upld_line = None
                    self._upload_request(path)
            elif len(self.upld_line) < 512:
                self.upld_line.append(b)
        elif self.tail == MAGIC_UPLD:
            self.upld_line = bytearray()

        if b == 0x0A:
            m = re.search(rb'sha1:\s+([0-9A-Fa-f]{40})', self.line)
            if m and self.pending_sha:
                remote = m.group(1).decode().upper()
                if remote == self.pending_sha:
                    self.msg('sha1 OK')
                else:
                    self.msg('sha1 MISMATCH (local %s)' % self.pending_sha)
                self.pending_sha = None
            self.line = bytearray()
        elif len(self.line) < 1024:
            self.line.append(b)

    # -- uploads
    def serve(self, sdpath, local):
        local = os.path.abspath(local)
        if not os.path.isfile(local):
            raise PicotermError('no such file: %s' % local)
        self.serve_map[sdpath] = local
        return local

    def _upload_request(self, sdpath):
        local = self.serve_map.pop(sdpath, None)
        if not local and sdpath in LEGACY_ENV:
            local = os.environ.get(LEGACY_ENV[sdpath])
        if not local:
            self.msg('console requests %s, but no file is registered for it '
                     '(Ctrl-T u in the terminal, or use the picoterm.py subcommands)' % sdpath)
            return
        threading.Thread(target=self._send_file, args=(sdpath, local), daemon=True).start()

    def _send_file(self, sdpath, local):
        try:
            with open(local, 'rb') as f:
                data = f.read()
        except OSError as e:
            self.msg('cannot read %s: %s' % (local, e))
            return
        self.pending_sha = hashlib.sha1(data).hexdigest().upper()
        self.msg('sending %s -> %s (%d bytes)' % (local, sdpath, len(data)))
        t0 = time.time()
        try:
            with self.wlock:
                self.port.write(MAGIC_SYNC + struct.pack('>I', len(data)))
                sent = 0
                next_report = 0
                while sent < len(data):
                    chunk = data[sent:sent + 512]
                    self.port.write(chunk)
                    sent += len(chunk)
                    if sent >= next_report:
                        sys.stderr.write('\r--- %d/%d bytes' % (sent, len(data)))
                        sys.stderr.flush()
                        next_report = sent + 16384
                self.port.drain()
        except OSError as e:
            self.msg('upload aborted: %s' % e)
            return
        dt = max(time.time() - t0, 0.001)
        self.msg('sent %d bytes in %.1fs (%.1f KiB/s)' % (len(data), dt, len(data) / dt / 1024))

    # -- control
    def mark(self):
        with self.cond:
            return self.log_base + len(self.log)

    def expect(self, pattern, start=None, timeout=10.0):
        """Wait for regex `pattern` in the output after absolute offset `start`.
        Returns (matched text, end offset, groups) or None on timeout."""
        rx = re.compile(pattern.encode('latin-1'), re.S)
        deadline = time.time() + timeout
        with self.cond:
            if start is None:
                start = self.log_base + len(self.log)
            while True:
                m = rx.search(self.log, max(start - self.log_base, 0))
                if m:
                    return (m.group(0).decode('latin-1'), self.log_base + m.end(),
                            [g.decode('latin-1') if g is not None else None for g in m.groups()])
                left = deadline - time.time()
                if left <= 0:
                    return None
                self.cond.wait(min(left, 0.5))

    def send(self, data):
        if isinstance(data, str):
            data = data.encode('latin-1')
        with self.wlock:
            self.port.write(data)

    def _wait_connected(self, timeout=10):
        if not self.connected.wait(timeout):
            raise PicotermError('Pico not connected')

    def reset(self):
        self._wait_connected()
        with self.wlock:
            set_baud(self.port.fd, RESET_BAUD)
            time.sleep(0.1)
            set_baud(self.port.fd, NORMAL_BAUD)
        self.msg('reset requested (baud %d)' % RESET_BAUD)

    def bootsel(self):
        self._wait_connected()
        with self.wlock:
            try:
                set_baud(self.port.fd, BOOTSEL_BAUD)
            except OSError:
                pass  # device may vanish immediately


# --------------------------------------------------------------------------
# control socket (the terminal is the server, the subcommands are clients)

class Server:
    def __init__(self, engine, path=SOCK_PATH):
        self.engine = engine
        self.path = path

    def start(self):
        if os.path.exists(self.path):
            try:
                s = socket.socket(socket.AF_UNIX)
                s.connect(self.path)
                s.close()
                raise PicotermError('another picoterm terminal is already running (%s)' % self.path)
            except (ConnectionRefusedError, FileNotFoundError):
                os.unlink(self.path)
        self.sock = socket.socket(socket.AF_UNIX)
        self.sock.bind(self.path)
        os.chmod(self.path, 0o600)
        self.sock.listen(4)
        threading.Thread(target=self._accept, daemon=True).start()

    def close(self):
        try:
            self.sock.close()
            os.unlink(self.path)
        except OSError:
            pass

    def _accept(self):
        while True:
            try:
                conn, _ = self.sock.accept()
            except OSError:
                return
            threading.Thread(target=self._client, args=(conn,), daemon=True).start()

    def _client(self, conn):
        e = self.engine
        f = conn.makefile('rwb')
        try:
            for line in f:
                try:
                    req = json.loads(line)
                    cmd = req['cmd']
                    if cmd == 'mark':
                        res = e.mark()
                    elif cmd == 'send':
                        e.send(req['data'])
                        res = True
                    elif cmd == 'reset':
                        e.reset()
                        res = True
                    elif cmd == 'bootsel':
                        e.bootsel()
                        res = True
                    elif cmd == 'serve':
                        res = e.serve(req['sdpath'], req['file'])
                    elif cmd == 'expect':
                        res = e.expect(req['pattern'], req.get('start'), req.get('timeout', 10))
                    elif cmd == 'msg':
                        e.msg(req['text'])
                        res = True
                    else:
                        raise PicotermError('unknown command %r' % cmd)
                    out = {'ok': True, 'result': res}
                except Exception as ex:
                    out = {'ok': False, 'error': str(ex)}
                f.write(json.dumps(out).encode() + b'\n')
                f.flush()
        except OSError:
            pass
        finally:
            conn.close()


class RemoteEngine:
    """Same interface as Engine, but talks to a running `picoterm.py term`."""

    def __init__(self, path=SOCK_PATH):
        self.sock = socket.socket(socket.AF_UNIX)
        self.sock.connect(path)
        self.f = self.sock.makefile('rwb')

    def _call(self, cmd, **kw):
        kw['cmd'] = cmd
        self.f.write(json.dumps(kw).encode() + b'\n')
        self.f.flush()
        line = self.f.readline()
        if not line:
            raise PicotermError('terminal closed the connection')
        res = json.loads(line)
        if not res['ok']:
            raise PicotermError(res['error'])
        return res['result']

    def mark(self):
        return self._call('mark')

    def send(self, data):
        if isinstance(data, bytes):
            data = data.decode('latin-1')
        return self._call('send', data=data)

    def reset(self):
        return self._call('reset')

    def bootsel(self):
        return self._call('bootsel')

    def serve(self, sdpath, local):
        return self._call('serve', sdpath=sdpath, file=os.path.abspath(local))

    def expect(self, pattern, start=None, timeout=10.0):
        r = self._call('expect', pattern=pattern, start=start, timeout=timeout)
        return tuple(r) if r else None

    def msg(self, text):
        print('--- ' + text, file=sys.stderr)
        try:
            self._call('msg', text=text)
        except PicotermError:
            pass

    def stop(self):
        self.sock.close()


def get_engine(args, echo=True):
    """Use the running terminal if there is one, else open the port directly."""
    try:
        return RemoteEngine()
    except (FileNotFoundError, ConnectionRefusedError):
        pass
    path = find_port(args.port)
    if path:
        users = port_users(path)
        if users and not args.force:
            raise PicotermError('%s is in use by: %s\nClose it (or run `picoterm.py term` instead of '
                            'minicom), or pass --force.' % (path, ', '.join(users)))
    out = None
    if echo:
        def out(data):
            sys.stdout.buffer.write(data)
            sys.stdout.buffer.flush()
    e = Engine(args.port, raw_out=out)
    e.start()
    return e


# --------------------------------------------------------------------------
# flows

def enter_console(e, do_reset=True, timeout=60):
    """Get minute into its interactive console, with an empty command line."""
    start = e.mark()
    if do_reset:
        e.reset()
        if not e.expect(r'minute loading|Autobooting in', start, 30):
            raise PicotermError('minute did not come up after reset')
    deadline = time.time() + timeout
    while time.time() < deadline:
        # '\' skips autoboot (new minute), opens the console from the menu,
        # or is just typed into an already open console (deleted below).
        mark = e.mark()
        e.send(b'\\')
        if e.expect(INTCON_PROMPT, mark, 1.0):
            time.sleep(0.3)
            e.send(b'\b' * 32)
            time.sleep(0.3)
            return
    raise PicotermError('could not reach minute\'s interactive console. If autoboot is on, the '
                    'installed minute can only skip it with POWER/EJECT; install the new fw.img '
                    'once to get the serial skip.')


def run_cmd(e, cmd):
    mark = e.mark()
    e.send(cmd.encode('latin-1') + b'\r')
    return mark


def upload(e, local, sdpath, cmd=None):
    with open(local, 'rb') as f:
        data = f.read()
    size = len(data)
    sha = hashlib.sha1(data).hexdigest().upper()
    e.serve(sdpath, local)
    mark = run_cmd(e, cmd or 'upload %s' % sdpath)
    r = e.expect(r'command not found|Transfer (complete!|failed\.)', mark, 60 + size / 2000)
    if not r:
        raise PicotermError('upload timed out')
    if 'not found' in r[0]:
        raise PicotermError('the installed minute has no `upload <path>` command yet; '
                        'install the new fw.img first (picoterm.py fw <fw.img>)')
    if 'failed' in r[0]:
        raise PicotermError('minute reported: Transfer failed')
    s = e.expect(r'sha1:\s+([0-9A-Fa-f]{40})', mark, 30)
    if not s:
        raise PicotermError('no sha1 from minute')
    if s[2][0].upper() != sha:
        raise PicotermError('sha1 mismatch: console %s, local %s' % (s[2][0], sha))
    e.msg('upload verified: %s (%d bytes, sha1 %s)' % (sdpath, size, sha))
    return mark


def sd_plugin_path(name):
    """Plugin name or full SD path -> full SD path."""
    return name if ':' in name else '%s/%s' % (PLUGIN_DIR, os.path.basename(name))


def remove(e, sdpath):
    mark = run_cmd(e, 'rm %s' % sdpath)
    r = e.expect(r'command not found|rm: (ok|fail)( -?\d+)? ' + re.escape(sdpath), mark, 10)
    if not r:
        raise PicotermError('no answer to `rm %s`' % sdpath)
    if 'not found' in r[0]:
        raise PicotermError('the installed minute has no `rm` command yet; install the new fw.img first')
    if r[2][0] == 'ok':
        e.msg('deleted %s' % sdpath)
    else:
        e.msg('could not delete %s (errno%s)' % (sdpath, r[2][1]))
    return r[2][0] == 'ok'


def list_dir(e, sddir):
    mark = run_cmd(e, 'ls %s' % sddir)
    r = e.expect(r'command not found|ls: (ok|fail) ', mark, 10)
    if not r:
        raise PicotermError('no answer to `ls %s`' % sddir)
    if 'not found' in r[0]:
        raise PicotermError('the installed minute has no `ls` command yet; install the new fw.img first')
    if r[2][0] == 'fail':
        raise PicotermError('minute could not open %s' % sddir)
    # entries are the "  name" lines between the echoed command and the status line
    text = e.expect(r'.*ls: ok', mark, 1)[0]
    return [l[2:] for l in re.split(r'\r?\n', text) if l.startswith('  ') and l.strip()]


def check_file(path):
    if not os.path.isfile(path):
        raise PicotermError('no such file: %s' % path)


def cmd_reset(args):
    e = get_engine(args, echo=False)
    e.reset()
    e.stop()


def cmd_send(args):
    e = get_engine(args)
    mark = run_cmd(e, ' '.join(args.text))
    if args.wait:
        e.expect(r'(?!)', mark, args.wait)  # just let output stream for a while
    else:
        time.sleep(0.2)
    e.stop()


def follow(e, args):
    if getattr(args, 'follow', False) and isinstance(e, Engine):
        try:
            while True:
                time.sleep(1)
        except KeyboardInterrupt:
            pass


def cmd_console(args):
    e = get_engine(args)
    enter_console(e, do_reset=not args.no_reset)
    e.msg('minute console ready')
    follow(e, args)
    e.stop()


def cmd_plugin(args):
    if not args.files and not args.rm:
        raise PicotermError('nothing to do: give plugin files and/or --rm NAME')
    if args.name and len(args.files) != 1:
        raise PicotermError('--name only works with a single plugin file')
    uploads = []
    for f in args.files:
        check_file(f)
        uploads.append((f, sd_plugin_path(args.name or f)))
    e = get_engine(args)
    enter_console(e, do_reset=not args.no_reset)
    failed = [n for n in args.rm if not remove(e, sd_plugin_path(n))]
    for local, sdpath in uploads:
        upload(e, local, sdpath)
    if failed and not args.force_boot:
        raise PicotermError('not booting, delete failed for: %s (use --force-boot)' % ', '.join(failed))
    if args.boot:
        mark = run_cmd(e, 'boot %d' % args.boot)
        if not e.expect(r'Booting %d:' % args.boot, mark, 10):
            raise PicotermError('minute did not accept `boot %d`' % args.boot)
        e.msg('booting menu entry %d' % args.boot)
    follow(e, args)
    e.stop()


def cmd_plugins(args):
    e = get_engine(args, echo=False)
    enter_console(e, do_reset=not args.no_reset)
    for name in list_dir(e, args.dir):
        print(name)
    e.stop()


def cmd_fw(args):
    check_file(args.file)
    e = get_engine(args)
    enter_console(e, do_reset=not args.no_reset)
    # `up` exists in old and new minute: writes sdmc:/fw.img, then reloads
    mark = upload(e, args.file, 'sdmc:/fw.img', cmd='up')
    if e.expect(r'Failed to load minute\.img', mark, 8):
        # old minute only reloads minute.img: reset instead, boot picks up fw.img
        e.msg('installed minute cannot reload fw.img, resetting instead')
        e.reset()
    follow(e, args)
    e.stop()


def find_rpi_rp2():
    for pattern in ('/run/media/*/RPI-RP2', '/media/*/RPI-RP2', '/media/RPI-RP2', '/mnt/RPI-RP2'):
        hits = glob.glob(pattern)
        if hits:
            return hits[0]
    return None


def cmd_flash_pico(args):
    uf2 = os.path.abspath(args.file)
    if not os.path.isfile(uf2):
        raise PicotermError('no such file: %s' % uf2)
    if find_port(args.port):
        e = get_engine(args, echo=False)
        e.bootsel()
        e.stop()
        print('--- Pico rebooting into BOOTSEL...', file=sys.stderr)
    deadline = time.time() + 15
    picotool = shutil.which('picotool')
    while time.time() < deadline:
        time.sleep(1)
        drive = find_rpi_rp2()
        if drive:
            shutil.copy(uf2, drive)
            os.sync()
            print('--- copied %s to %s' % (uf2, drive), file=sys.stderr)
            return
        if picotool:
            r = subprocess.run([picotool, 'load', '-x', uf2], capture_output=True, text=True)
            if r.returncode == 0:
                print(r.stdout, file=sys.stderr)
                return
    raise PicotermError('Pico BOOTSEL drive not found and picotool failed (permissions?). Try:\n'
                    '  sudo picotool load -x %s' % uf2)


# --------------------------------------------------------------------------
# interactive terminal

TERM_HELP = """picoterm: Ctrl-] quit | Ctrl-T then:
      r  reset console (Pico NRST)      i  minute interactive console ('\\')
      u  upload a file (asks for paths) q  quit
      h  this help                      Ctrl-T  send Ctrl-T
    control socket: %s"""


def cmd_term(args):
    stdin_fd = sys.stdin.fileno()
    if not os.isatty(stdin_fd):
        raise PicotermError('term needs a terminal')
    path = find_port(args.port)
    if path:
        users = port_users(path)
        if users and not args.force:
            raise PicotermError('%s is in use by: %s (close minicom first, or --force)'
                            % (path, ', '.join(users)))

    def out(data):
        os.write(sys.stdout.fileno(), data)

    e = Engine(args.port, raw_out=out, msg_eol='\r\n')
    server = Server(e)
    server.start()
    old = termios.tcgetattr(stdin_fd)
    try:
        tty.setraw(stdin_fd)
        e.msg(TERM_HELP % SOCK_PATH)
        e.start(wait=False)
        menu = False
        while True:
            data = os.read(stdin_fd, 1024)
            if not data:
                break
            outbuf = bytearray()
            quit_now = False
            for b in data:
                if menu:
                    menu = False
                    c = chr(b).lower()
                    if b == 0x14:
                        outbuf.append(b)
                    elif c == 'r':
                        try:
                            e.reset()
                        except Exception as ex:
                            e.msg('reset failed: %s' % ex)
                    elif c == 'i':
                        outbuf += b'\\'
                    elif c == 'u':
                        termios.tcsetattr(stdin_fd, termios.TCSADRAIN, old)
                        try:
                            term_upload(e)
                        finally:
                            tty.setraw(stdin_fd)
                    elif c == 'q':
                        quit_now = True
                        break
                    else:
                        e.msg(TERM_HELP % SOCK_PATH)
                elif b == 0x14:
                    menu = True
                elif b == 0x1D:
                    quit_now = True
                    break
                elif b == 0x7F:
                    outbuf.append(0x08)  # minute only knows '\b'
                else:
                    outbuf.append(b)
            if outbuf:
                try:
                    e.send(bytes(outbuf))
                except OSError as ex:
                    e.msg('not sent: %s' % ex)
            if quit_now:
                break
    finally:
        termios.tcsetattr(stdin_fd, termios.TCSADRAIN, old)
        server.close()
        e.stop()
        print('\n--- exit', file=sys.stderr)


def term_upload(e):
    try:
        sys.stderr.write('\n--- local file: ')
        local = input().strip()
        if not local:
            return
        local = os.path.expanduser(local)
        if local.endswith('.ipx'):
            default = '%s/%s' % (PLUGIN_DIR, os.path.basename(local))
        else:
            default = 'sdmc:/%s' % os.path.basename(local)
        sys.stderr.write('--- SD path [%s]: ' % default)
        sdpath = input().strip() or default
        e.serve(sdpath, local)
        e.msg('registered %s -> %s; sending `upload %s` (minute console must be open)'
              % (local, sdpath, sdpath))
        e.send(('upload %s\r' % sdpath).encode('latin-1'))
    except (EOFError, KeyboardInterrupt):
        pass
    except Exception as ex:
        e.msg('upload: %s' % ex)


# --------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', default=DEFAULT_PORT,
                    help='serial port (default: $PICOTERM_PORT, else %s)' % PORT_GLOB)
    ap.add_argument('--force', action='store_true', help='open the port even if another program has it')
    sub = ap.add_subparsers(dest='cmd', required=True)

    sub.add_parser('term', help='interactive terminal (replaces minicom)').set_defaults(fn=cmd_term)
    sub.add_parser('reset', help='reset the console via the Pico').set_defaults(fn=cmd_reset)

    p = sub.add_parser('send', help="type a line into minute's console")
    p.add_argument('text', nargs='+')
    p.add_argument('--wait', type=float, default=0, help='seconds to show output afterwards')
    p.set_defaults(fn=cmd_send)

    p = sub.add_parser('console', help="(reset and) open minute's interactive console")
    p.add_argument('--no-reset', action='store_true')
    p.add_argument('-f', '--follow', action='store_true')
    p.set_defaults(fn=cmd_console)

    p = sub.add_parser('plugin', help='upload and/or delete stroopwafel plugins, then boot')
    p.add_argument('files', nargs='*', metavar='file.ipx')
    p.add_argument('--rm', action='append', default=[], metavar='NAME',
                   help='delete this plugin first (name in %s, or full SD path); repeatable' % PLUGIN_DIR)
    p.add_argument('--name', help='file name on SD for a single upload (default: basename)')
    p.add_argument('--force-boot', action='store_true', help='boot even if a delete failed')
    p.add_argument('--boot', type=int, default=DEFAULT_BOOT_ENTRY,
                   help='main menu entry to boot afterwards, 0 = stay in minute (default %(default)s)')
    p.add_argument('--no-reset', action='store_true', help='minute is already running')
    p.add_argument('-f', '--follow', action='store_true', help='keep showing output')
    p.set_defaults(fn=cmd_plugin)

    p = sub.add_parser('plugins', help='list the plugins on SD')
    p.add_argument('--dir', default=PLUGIN_DIR)
    p.add_argument('--no-reset', action='store_true', help='minute is already running')
    p.set_defaults(fn=cmd_plugins)

    p = sub.add_parser('fw', help='upload minute as sdmc:/fw.img and reload it')
    p.add_argument('file')
    p.add_argument('--no-reset', action='store_true', help='minute is already running')
    p.add_argument('-f', '--follow', action='store_true', help='keep showing output')
    p.set_defaults(fn=cmd_fw)

    p = sub.add_parser('flash-pico', help='reboot the Pico to BOOTSEL and flash a .uf2')
    p.add_argument('file', nargs='?', default=DEFAULT_UF2)
    p.set_defaults(fn=cmd_flash_pico)

    args = ap.parse_args()
    try:
        args.fn(args)
    except PicotermError as ex:
        print('picoterm: %s' % ex, file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        sys.exit(130)


if __name__ == '__main__':
    main()
