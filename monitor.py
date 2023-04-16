# pip3 install pyftdi
from pyftdi.ftdi import Ftdi, FtdiError
from pyftdi import serialext
from serial.serialutil import SerialException
import sys
import time

def hex_dump(d):
     idx = 0
     s = ""
     for val in d:
          s += ('%02x'%val) + " "
          idx += 1
          if idx == 16:
               s += "\n"
               idx = 0
     print (s)

Ftdi.show_devices()

port = serialext.serial_for_url('ftdi://ftdi:2232:ibrEq5Ay/2', baudrate=1000000)

#f = open("log-" + time.ctime() + ".txt", "w")

is_serial = False
is_text_serial = False
bits = 0
bval = 0
serial_data = []
serial_data_nowipe = []

last_data = -1
data = [-1]
while True:

     try:
          last_data = data[0]
          data = port.read(1)
          if not is_text_serial:
               print ("...",hex(data[0]))
          #f.write(hex(data[0]) + "\n")
          serial_data_nowipe += [data[0]]
          serial_data_nowipe = serial_data_nowipe[-16:]
          if data[0] == 0x55 and last_data == 0xaa and not is_serial:
               print("")
               #f.write("\n")
               #f.flush()
               is_serial = False
               serial_data = []

          #if data[0] == 0x88:
          #     is_serial = False
          #     serial_data = []

          if data[0] == 0x8F and not is_serial:
               is_serial = not is_serial
               print ("Starting serial!")
               hex_dump (serial_data)
               #serial_data = []
          elif is_text_serial:
               if data[0] == ord('\t') or data[0] == ord('\n') or data[0] == ord('\r') or (data[0] >= ord(' ') and data[0] <= ord('~')):
                    if data[0] == ord('\n'):
                         port.write(b'\x55')
                    if data[0] == ord('\r') and serial_data_nowipe[-2] != ord('\n'):
                         sys.stdout.write('\n')
                    else:
                         sys.stdout.write(chr(data[0]))
               else:
                    sys.stdout.write("\\x" + ('%02x'%data[0]))
          elif is_serial:
               serial_data += [data[0]]
               hex_dump (serial_data)
               if serial_data[-8:] == [0x55, 0xaa, 0x55, 0xaa, 0x55, 0xaa, 0x55, 0xaa]:
                    serial_data = []
               elif serial_data[-2:] == [0x20, 0x25]:
                    serial_data = []
               elif serial_data[-2:] == [0xaa, 0x55] and len(serial_data) > 9 and serial_data[-9] == 0x55:
                    serial_data = []

          if serial_data_nowipe[-12:] == [0x55, 0xaa, 0x55, 0xaa, 0x55, 0xaa, 0x55, 0xaa, 0xf0, 0x0f, 0xca, 0xfe]:
               is_serial = True
               is_text_serial = True
               serial_data = []
          elif serial_data_nowipe[-12:] == [0x55, 0xaa, 0x55, 0xaa, 0x55, 0xaa, 0x55, 0xaa, 0xbe, 0xef, 0xca, 0xfe]:
               is_serial = True
               is_text_serial = False
               serial_data = []

     except FtdiError as e:
          print(e)
          exit(-1)
     except KeyboardInterrupt:
          exit(-1)
     except Exception as e:
          print(e)
          exit(-1)