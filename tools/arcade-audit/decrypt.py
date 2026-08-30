import zipfile, numpy as np, hashlib
import os as _os

_HERE = _os.path.dirname(_os.path.abspath(__file__))
# Repo root = <repo>/tools/arcade-audit/.. /.. — works in any worktree.
REPO = _os.environ.get("ARCADE_AUDIT_REPO") or _os.path.abspath(_os.path.join(_HERE, "..", ".."))
AFS_PATH = _os.environ.get("ARCADE_AUDIT_AFS") or _os.path.expanduser(
    "~/Library/Application Support/CrowdedStreet/3S-ARM/resources/SF33RD.AFS")
ROM_PATH = _os.environ.get("ARCADE_AUDIT_ROM") or _os.path.join(_HERE, "rom.bin")
ZIP_PATH = _os.environ.get("ARCADE_AUDIT_ROMZIP") or _os.path.expanduser(
    "~/Library/Application Support/CrowdedStreet/3S-ARM/resources/sfiii3nr1.zip")

z=zipfile.ZipFile(ZIP_PATH)
s=[np.frombuffer(z.read('sfiii3-simm1.%d'%i),dtype=np.uint8).astype(np.uint32) for i in range(4)]
for i in range(4):
    print(i, hashlib.sha256(z.read('sfiii3-simm1.%d'%i)).hexdigest())
N=2*1024*1024
cur=(s[0]<<24)|(s[1]<<16)|(s[2]<<8)|s[3]
BASE=0x6000000; K1=0xA55432B4; K2=0x0C129981
def rol(v,n):
    v=v&0xFFFF
    aux=v>>(16-n)
    return ((v<<n)|aux)%0x10000
def rotxor(val,xorval):
    val=val&0xFFFF; xorval=xorval&0xFFFF
    res=(val+rol(val,2))
    res=rol(res,4)^(res&(val^xorval))
    return res
addr=(BASE+(np.arange(N,dtype=np.uint64)*4)).astype(np.uint64)
a=(addr^np.uint64(K1))
val=((a&np.uint64(0xFFFF))^np.uint64(0xFFFF)).astype(np.uint64)
val=rotxor(val,np.uint64(K2&0xFFFF)).astype(np.uint64)
val=val^((a>>np.uint64(16))^np.uint64(0xFFFF))
val=rotxor(val,np.uint64(K2>>16)).astype(np.uint64)
val=val^((a&np.uint64(0xFFFF))^np.uint64(K2&0xFFFF))
val=val&np.uint64(0xFFFF)
masked=(val|(val<<np.uint64(16)))&np.uint64(0xFFFFFFFF)
V=(cur.astype(np.uint64)^masked).astype(np.uint32)
out=V.astype('>u4').tobytes()
open(ROM_PATH,'wb').write(out)
print(len(out))
