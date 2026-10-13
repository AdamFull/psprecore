import re,sys
d=open(sys.argv[1],'rb').read()
out=open(sys.argv[2],'w',encoding='utf8')
for m in re.finditer(rb'[\x20-\x7e]{5,}',d):
    out.write(f"{m.start():08x} {m.group().decode()}\n")
