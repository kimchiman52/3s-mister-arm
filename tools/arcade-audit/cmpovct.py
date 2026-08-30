import struct
import os as _os

_HERE = _os.path.dirname(_os.path.abspath(__file__))
# Repo root = <repo>/tools/arcade-audit/.. /.. — works in any worktree.
REPO = _os.environ.get("ARCADE_AUDIT_REPO") or _os.path.abspath(_os.path.join(_HERE, "..", ".."))
AFS_PATH = _os.environ.get("ARCADE_AUDIT_AFS") or _os.path.expanduser(
    "~/Library/Application Support/CrowdedStreet/3S-ARM/resources/SF33RD.AFS")
ROM_PATH = _os.environ.get("ARCADE_AUDIT_ROM") or _os.path.join(_HERE, "rom.bin")
ZIP_PATH = _os.environ.get("ARCADE_AUDIT_ROMZIP") or _os.path.expanduser(
    "~/Library/Application Support/CrowdedStreet/3S-ARM/resources/sfiii3nr1.zip")

ps2=open('ps2_ryu_chd.bin','rb').read()
rom=open(ROM_PATH,'rb').read()
PS2_OVCT=0xF4A4; PS2_N=56
ARC_OVCT=0x2035CC; ARC_N=54
ogt=[int(x) for x in open('ogt.txt').read().split(',')]
def ps2p(i):
    o=PS2_OVCT+i*16
    hx,hy=struct.unpack_from('<hh',ps2,o)
    colmd,colcd,prio,flip,timer,disp=struct.unpack_from('<6B',ps2,o+4)
    mts,nix,ch=struct.unpack_from('<hHH',ps2,o+10)
    return dict(hx=hx,hy=hy,colmd=colmd,colcd=colcd,prio=prio,flip=flip,timer=timer,disp=disp,mts=mts,nix=nix,ch=ch)
def arcp(i):
    o=ARC_OVCT+i*16
    hx,hy=struct.unpack_from('>hh',rom,o)
    colmd,colcd,prio,flip,timer,disp=struct.unpack_from('>6B',rom,o+4)
    mts,nix,ch=struct.unpack_from('>hHH',rom,o+10)
    return dict(hx=hx,hy=hy,colmd=colmd,colcd=colcd,prio=prio,flip=flip,timer=timer,disp=disp,mts=mts,nix=nix,ch=ch)
BEH=['hx','hy','colmd','prio','flip','timer','disp','nix']
common=min(PS2_N,ARC_N)
print("arcade_count=%d ps2_count=%d common_count=%d"%(ARC_N,PS2_N,common))
bad=[]
for i in range(common):
    a,p=arcp(i),ps2p(i)
    d=[k for k in BEH if a[k]!=p[k]]
    if d: bad.append((i,d,a,p))
print("overlap_behavior_matches failures over common prefix:",len(bad))
for x in bad[:10]: print("  part",x[0],"fields",x[1],"arc",{k:x[2][k] for k in x[1]},"ps2",{k:x[3][k] for k in x[1]})
print()
print("part | arcade_char  ps2_char(applied) grp(ps2) n(ps2)  | arc_mts ps2_mts | arc_colcd ps2_colcd")
for i in range(common):
    a,p=arcp(i),ps2p(i)
    g=ogt[p['ch']] if p['ch']<len(ogt) else None
    n1=texn=''
    print(" %3d | 0x%04X       0x%04X            %-6s %-6s | %5d %5d | %5d %5d %s"%(
        i,a['ch'],p['ch'],str(g),
        (p['ch']-2592) if g==3 else '-', a['mts'],p['mts'],a['colcd'],p['colcd'],
        '' if p['ch']<len(ogt) else 'PS2_CHAR_OOB!'))
print()
print("PS2 parts 54,55 (beyond arcade table, unused by runtime):")
for i in (54,55): print("  ",i,ps2p(i))
