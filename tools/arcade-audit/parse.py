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

rom=open(ROM_PATH,'rb').read()
BASE=0x6000000
def offsets(off,size):
    ents=[];p=off
    while True:
        v=struct.unpack_from('>I',rom,p)[0];p+=4
        if v==0: break
        ents.append(v-BASE-off)
    return ents
def parse_script(tbl_off,tbl_size,idx,verbose=True):
    offs=offsets(tbl_off,tbl_size)
    so=sorted(set(offs))
    start=offs[idx]-8
    # end = next distinct offset -8 or table size
    nxt=[o for o in so if o>offs[idx]]
    end=(nxt[0]-8) if nxt else tbl_size
    p=tbl_off+start
    cgd=struct.unpack_from('>h',rom,p)[0]
    hdr=rom[p+2:p+8]
    out=[]
    q=p+8
    limit=tbl_off+end
    while q<limit:
        code=struct.unpack_from('>H',rom,q)[0]
        if code<0x100:
            koc,ix,pat=struct.unpack_from('>hhh',rom,q+2)
            out.append(('CMD',code,koc,ix,pat))
            q+=8+max(cgd*4-8,0)
        else:
            cg_ctr=code>>8; cg_type=code&0xFF
            cg_se,cg_olc,cg_num=struct.unpack_from('>HHH',rom,q+2)
            rec=dict(ctr=cg_ctr,type=cg_type,se=cg_se,olc=cg_olc,num=cg_num)
            q2=q+8
            if cgd>=4:
                cg_att,cg_hit=struct.unpack_from('>hH',rom,q2)
                ext,canc,eff,eftype=struct.unpack_from('>BBBB',rom,q2+4)
                rec.update(att=cg_att,hit=cg_hit,ext=ext,canc=canc,eff=eff,eftype=eftype)
                q2+=8
            if cgd==6:
                z,riv,addxy=struct.unpack_from('>HHH',rom,q2)
                nix,stat=struct.unpack_from('>BB',rom,q2+6)
                rec.update(zoom=z,rival=riv,addxy=addxy,nix=nix,stat=stat)
                q2+=8
            out.append(('CELL',rec))
            q=q2
    return cgd,hdr,out,start,end
SACA=(0x20669C,0x2748)
for idx in (40,41,42,36,4,5,6,7):
    cgd,hdr,cells,st,en=parse_script(*SACA,idx)
    print("=== saca idx",idx,"cgd_type",cgd,"hdr",hdr.hex(),"range",hex(st),hex(en))
    for c in cells:
        if c[0]=='CMD': print("   CMD code=%d koc=%d ix=%d pat=%d"%c[1:])
        else:
            r=c[1]
            print("   CELL type=%d ctr=%d se=0x%04x olc=0x%04x num=0x%04x eff=%s eftype=%s att=%s hit=%s"%(
                r['type'],r['ctr'],r['se'],r['olc'],r['num'],r.get('eff'),r.get('eftype'),r.get('att'),r.get('hit')))
