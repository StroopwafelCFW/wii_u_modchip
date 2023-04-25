print ("const uint32_t serial_LUT[256] = {")
working_str = ""
for i in range(0, 256):
    if i % 4 == 0:
        print (working_str)
        working_str = "\t"
    val = 0
    for j in range(0, 8):
        if i & (1<<j):
            val |= (0xF << j*4)
    working_str += ("0x%08x," % (val))
print (working_str)
print("};")