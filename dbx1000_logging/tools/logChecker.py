import sys
import os

LOG_NUM = 2

if len(sys.argv) < 2:
    print('Usage: %s logFile' % sys.argv[0])
    sys.exit(0)

logfile = sys.argv[1]



LV_MAX = os.path.getsize(logfile)

f = open(logfile, "rb")
counter = 0
offset = 0
while True:
    print('Log', counter)
    header = f.read(8)
    offset += 8
    if len(header) == 0:
        break
    assert(len(header) == 8)
    checksum = int.from_bytes(header[:4], byteorder='little', signed=False)
    print(hex(checksum))
    assert(checksum == 0xbeef or checksum == 0x7f)
    size = int.from_bytes(header[4:], byteorder='little', signed=False)
    size_aligned = size
    print(size)
    if size % 64 != 0:
        size_aligned = size + 64 - size % 64
    content = f.read(size_aligned-8)
    
    if size_aligned == 64:
        print((header + content).hex())
    offset += size_aligned - 8
    #lv_i = [0] * LOG_NUM
    #for i in range(LOG_NUM):
    #    lv_i = int.from_bytes(content[size-i*8-8:size-i*8], byteorder="little", signed=False)
    #    assert(lv_i < LV_MAX)
    print(size_aligned, offset)
    #assert(len(content) > LOG_NUM * 8)

    counter += 1
    if counter % 10000 == 0:
        print(counter)

f.close()
print("In total %d txns." % counter)
