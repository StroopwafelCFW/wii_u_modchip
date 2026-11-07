# wii_u_modchip

A (WIP) Wii U modchip based on de_Fuse, a flaw in the Wii U's OTP eFuse readout state machine.

## How does it work?
A full writeup is available [here](https://douevenknow.us/post/714056575412764672/defuse-the-one-true-pwn). However, the short version is as follows:

In order to accommodate eFuse-based JTAG lockout (and due to other considerations), eFuse bits must be buffered into a register file immediately following NRST, before the internal reset can be released. The eFuse sense state machine latches at a rate of 4 bits per cycle, directly off the 27MHz XTALCLK. Every other rising edge, a byte is written into the register file, starting from the least significant byte of the current u32.

An internal counter is used to keep track of the remaining bytes to be read into the register file. While the eFuse register file is reset to zero with NRST, the internal counter is not: By asserting NRST after N bytes have been read, only 0x400-N bytes will be read on the subsequent boot.

By asserting NRST just before the final byte has been read (1830 cycles), all eFuses will read entirely zero, including the JTAG lockout fuse. This allows trivial, unsigned and unencrypted boot1 execution, with no SEEPROM anti-rollback.

![NRST waveform](docs/nrst_waveform.png)

## Serial-only Pico firmware (e.g. for ISFShax consoles)

Build and flash the `pico_serial` firmware. It's the same serial bridge, but without de_Fuse.
Baud 1201 resets the console and baud 1200 reboots the Pico into BOOTSEL.

    (cd pico_defuse && ./build.sh pico_serial)   # or: ./picoterm.py flash-pico  (uses pico_defuse/build/pico_serial.uf2)


## Dev workflow with `picoterm.py`

Run `./picoterm.py term` in place of minicom. Only one program can read the Pico's tty,
so the terminal owns it. The other subcommands drive the console through its control
socket (`$XDG_RUNTIME_DIR/picoterm.sock`, or `$PICOTERM_SOCK`). If no terminal is running,
they open the port themselves.

    ./picoterm.py term                          # Ctrl-T h for help, Ctrl-] to quit
    ./picoterm.py reset                         # Pico pulses NRST -> ISFShax -> minute
    ./picoterm.py plugin a.ipx b.ipx            # reset, upload to sdmc:/wiiu/ios_plugins/, boot menu entry 3
    ./picoterm.py plugin --rm old.ipx new.ipx   # delete and upload in one go
    ./picoterm.py plugin a.ipx --boot 0         # ...and stay in minute
    ./picoterm.py plugins                       # list sdmc:/wiiu/ios_plugins
    ./picoterm.py fw fw.img                     # reset, upload sdmc:/fw.img, reload it
    ./picoterm.py console                       # reset and open minute's interactive console
    ./picoterm.py send "smc read 0x40"          # type a line into minute's console

Options:
* `--no-reset` when minute is already running.
* `-f` to keep following the output.
* `--port` (or `$PICOTERM_PORT`) to override the autodetected
  `/dev/serial/by-id/usb-Raspberry_Pi_Pico_*`.

Uploading needs minute's `upload <path>` command, and deleting and listing need its `rm` and `ls`
commands, so install a current minute `fw.img` first. `picoterm.py fw` also works with older minute builds.
