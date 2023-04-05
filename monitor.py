# pip3 install pyftdi
from pyftdi.ftdi import Ftdi, FtdiError
from pyftdi import serialext
from serial.serialutil import SerialException
import sys
import time

Ftdi.show_devices()

port = serialext.serial_for_url('ftdi://ftdi:2232:ibrEq5Ay/2', baudrate=1000000)

f = open("log-" + time.ctime() + ".txt", "w")

last_data = -1
data = [-1]
while True:

     try:
          last_data = data[0]
          data = port.read(1)
          print (hex(data[0]))
          f.write(hex(data[0]) + "\n")
          if data[0] == 0x55 and last_data == 0xaa:
               print("")
               f.write("\n")
               f.flush()
     except FtdiError as e:
          print(e)
          exit(-1)
     except KeyboardInterrupt:
          exit(-1)
     except Exception as e:
          print(e)
          exit(-1)