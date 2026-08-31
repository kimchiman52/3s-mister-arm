import struct,sys
import os as _os

_HERE = _os.path.dirname(_os.path.abspath(__file__))
# Repo root = <repo>/tools/arcade-audit/.. /.. — works in any worktree.
REPO = _os.environ.get("ARCADE_AUDIT_REPO") or _os.path.abspath(_os.path.join(_HERE, "..", ".."))
AFS_PATH = _os.environ.get("ARCADE_AUDIT_AFS") or _os.path.expanduser(
    "~/Library/Application Support/CrowdedStreet/3S-ARM/resources/SF33RD.AFS")
ROM_PATH = _os.environ.get("ARCADE_AUDIT_ROM") or _os.path.join(_HERE, "rom.bin")
ZIP_PATH = _os.environ.get("ARCADE_AUDIT_ROMZIP") or _os.path.expanduser(
    "~/Library/Application Support/CrowdedStreet/3S-ARM/resources/sfiii3nr1.zip")

P=AFS_PATH
f=open(P,'rb')
magic=f.read(4)
print("magic bytes:",magic, "== 'AFS\\0':", magic==b'AFS\x00')
cnt=struct.unpack('<I',f.read(4))[0]
print("entry_count:",cnt)
ents=[]
for i in range(cnt):
    o,s=struct.unpack('<II',f.read(8))
    ents.append((o,s))
APFN=1468            # texgrpdat[3].apfn  (Ryu)
TO_CHD=0x178F38      # texgrpdat[3].to_chd
off,size=ents[APFN]
print("AFS entry[%d]: offset=0x%X size=%u (0x%X)"%(APFN,off,size,size))
print("texgrpdat[3].use (declared) = 1630652 ; AFS size == use ?", size==1630652)
ps2_size=size-TO_CHD
print("ps2_size = size - to_chd = %u (0x%X)"%(ps2_size,ps2_size))
f.seek(off+TO_CHD)
ps2=f.read(ps2_size)
assert len(ps2)==ps2_size
SEC=['NMCA','DMCA','BTCA','CACA','CUCA','ATCA','SACA','EXCA','CBCA','YUCA','STXY','MVXY','SERND','OVCT','OVIX','RICT','HIIT','BODA','HANA','CATA','CAUA','ATTA','HOSA','ATIT','PROT']
N=25
offs=list(struct.unpack_from('<25I',ps2,0))
print("--- PS2 25-entry section header ---")
for i,(n,o) in enumerate(zip(SEC,offs)): print("  %2d %-6s 0x%08X (%u)"%(i,n,o,o))
def span(section):
    hdr=N*4
    start=offs[section]; end=ps2_size
    if start<hdr or start>=ps2_size: return None
    for i in range(N):
        if offs[i]>start and offs[i]<end: end=offs[i]
    return start,end-start
for sec,elem in ((13,16),(14,8)):
    r=span(sec)
    print("PS2 %s span: start=0x%X size=%u  count=%s  size%%elem=%d"%(SEC[sec],r[0],r[1],r[1]//elem,r[1]%elem))
open('ps2_ryu_chd.bin','wb').write(ps2)
