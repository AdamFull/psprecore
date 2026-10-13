import struct,sys
d=open(sys.argv[1],'rb').read()
h=struct.unpack('<16sHHIIIIIHHHHHH',d[:52])
_,et,em,ev,entry,phoff,shoff,flags,ehs,phes,phn,shes,shn,shstr=h
print(f"type={et:#x} mach={em} entry={entry:#x} flags={flags:#x} phn={phn} shn={shn}")
for i in range(phn):
    p=struct.unpack('<8I',d[phoff+i*32:phoff+i*32+32]); print(" PH type=%#x off=%#x vaddr=%#x paddr=%#x filesz=%#x memsz=%#x flags=%#x align=%#x"%p)
secs=[]
for i in range(shn):
    secs.append(struct.unpack('<10I',d[shoff+i*40:shoff+i*40+40]))
so=secs[shstr][4]
def nm(o): return d[so+o:d.index(b'\0',so+o)].decode()
S={}
for s in secs:
    n=nm(s[0]); S[n]=s
    print(f" {n:28s} type={s[1]:#x} addr={s[3]:#8x} off={s[4]:#8x} size={s[5]:#8x}")
# module info
mi=S.get('.rodata.sceModuleInfo')
if mi:
    o=mi[4]; attr,ver,name,gp,ent,ente,stub,stube=struct.unpack('<HH28sIIIII',d[o:o+52])
    print("MODINFO",attr,hex(ver),name.rstrip(b'\0'),hex(gp),hex(ent),hex(ente),hex(stub),hex(stube))
    def v2o(a):
        for s in secs:
            if s[3] and s[3]<=a<s[3]+s[5] and s[1]!=8: return s[4]+a-s[3]
    def cstr(a):
        o=v2o(a); return d[o:d.index(b'\0',o)].decode()
    o=v2o(stub); e=v2o(stube); tot=0
    print("IMPORTS:")
    while o<e:
        nameaddr,ver,attr,esz,vc,fc,nids,funcs=struct.unpack('<IHHBBHII',d[o:o+20])
        print(f"  {cstr(nameaddr):28s} funcs={fc} vars={vc}"); tot+=fc
        o+=esz*4
    print(" total imported funcs",tot)
