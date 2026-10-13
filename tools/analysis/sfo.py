import struct,sys
d=open(sys.argv[1],'rb').read()
magic,ver,kt,dt,n=struct.unpack('<4sIIII',d[:20])
for i in range(n):
    ko,fmt,ln,mx,do=struct.unpack('<HHIII',d[20+i*16:36+i*16])
    k=d[kt+ko:d.index(b'\0',kt+ko)].decode()
    v=d[dt+do:dt+do+ln]
    print(k, struct.unpack('<I',v)[0] if fmt==0x404 else v.rstrip(b'\0').decode('utf8','replace'))
