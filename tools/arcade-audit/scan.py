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

rom=open(ROM_PATH,'rb').read()
BASE=0x6000000
RYU={ 'nmca':(0x1F9268,0x16D0),'dmca':(0x1FA938,0x217C),'btca':(0x1FCAB4,0x1584),
 'caca':(0x1FE038,0x724),'cuca':(0x1FE75C,0x2D24),'atca':(0x201480,0x2114),
 'saca':(0x20669C,0x2748),'exca':(0x20476C,0x1F30),'cbca':(0x208DE4,0x9D0),
 'yuca':(0x362B64,0x6E8)}
KOC={0:'nmca',1:'dmca',2:'caca',3:'cuca',4:'atca',5:'saca',6:'btca',7:'exca',8:'cbca',9:'yuca'}
def offsets(off,size):
    ents=[];p=off
    while True:
        v=struct.unpack_from('>I',rom,p)[0];p+=4
        if v==0: break
        ents.append(v-BASE-off)
    return ents
tabs={k:offsets(*v) for k,v in RYU.items()}
for k in tabs: print(k,"entries",len(tabs[k]))
def parse(name,idx):
    off,size=RYU[name]; offs=tabs[name]
    so=sorted(set(offs)); start=offs[idx]-8
    nxt=[o for o in so if o>offs[idx]]
    end=(nxt[0]-8) if nxt else size
    p=off+start
    cgd=struct.unpack_from('>h',rom,p)[0]
    out=[];q=p+8;limit=off+end
    if cgd not in (1,2,4,6): return cgd,[]
    while q<limit:
        code=struct.unpack_from('>H',rom,q)[0]
        if code<0x100:
            koc,ix,pat=struct.unpack_from('>hhh',rom,q+2)
            out.append(('CMD',code,koc,ix,pat)); q+=8+max(cgd*4-8,0)
        else:
            rec={'ctr':code>>8,'type':code&0xFF}
            se,olc,num=struct.unpack_from('>HHH',rom,q+2); rec.update(se=se,olc=olc,num=num)
            q2=q+8
            if cgd>=4:
                att,hit=struct.unpack_from('>hH',rom,q2)
                ext,canc,eff,eftype=struct.unpack_from('>BBBB',rom,q2+4)
                rec.update(att=att,hit=hit,ext=ext,eff=eff,eftype=eftype); q2+=8
            if cgd==6: q2+=8
            out.append(('CELL',rec)); q=q2
    return cgd,out
issues=[]
JUMPS={3:'jmp',4:'jpss',5:'jsr'}
for name in RYU:
    for i in range(len(tabs[name])):
        cgd,cells=parse(name,i)
        for c in cells:
            if c[0]=='CMD':
                _,code,koc,ix,pat=c
                if code>=125: issues.append((name,i,'CODE_OOB',code))
                if code in JUMPS:
                    if koc<0 or koc>=12: issues.append((name,i,'KOC_OOB',code,koc,ix))
                    elif koc in KOC:
                        n=len(tabs[KOC[koc]])
                        if ix<0 or ix>=n: issues.append((name,i,'IDX_OOB',JUMPS[code],KOC[koc],ix,n))
                    else: issues.append((name,i,'KOC_UNSET',koc,ix))
                if code==43:
                    if koc<0 or koc>=59: issues.append((name,i,'EXEC_OOB',koc,ix))
                    elif koc==2 and ix>=243: issues.append((name,i,'TAMA_OOB',ix))
                    elif koc==13 and ix>=69: issues.append((name,i,'SASIGN_OOB',ix))
            else:
                r=c[1]
                if 'eff' in r and r['eff']:
                    if r['eff']>=59: issues.append((name,i,'CGEFF_OOB',r['eff'],r['eftype']))
                    elif r['eff']==2 and r['eftype']>=243: issues.append((name,i,'TAMA_OOB',r['eftype']))
                    elif r['eff']==13 and r['eftype']>=69: issues.append((name,i,'SASIGN_OOB',r['eftype']))
print("ISSUES:",len(issues))
for x in issues[:80]: print(x)
