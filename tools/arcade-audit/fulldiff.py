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

# ---------- arcade ----------
rom=open(ROM_PATH,'rb').read(); BASE=0x6000000
ARC={'nmca':(0x1F9268,0x16D0),'dmca':(0x1FA938,0x217C),'btca':(0x1FCAB4,0x1584),'caca':(0x1FE038,0x724),
 'cuca':(0x1FE75C,0x2D24),'atca':(0x201480,0x2114),'saca':(0x20669C,0x2748),'exca':(0x20476C,0x1F30),
 'cbca':(0x208DE4,0x9D0),'yuca':(0x362B64,0x6E8)}
def a_offs(off,size):
    e=[];p=off
    while True:
        v=struct.unpack_from('>I',rom,p)[0];p+=4
        if v==0:break
        e.append(v-BASE-off)
    return e
A={k:a_offs(*v) for k,v in ARC.items()}
def remap(v):
    if v<0x400: return v
    d=-0x01E0
    if 0x7082<=v<=0x7090: d=-0x62C6
    a=v+d
    return v if (a<0 or a>0xFFFF) else a
def a_parse(name,idx):
    off,size=ARC[name]; ents=A[name]; so=sorted(set(ents))
    start=ents[idx]-8; nxt=[o for o in so if o>ents[idx]]
    end=(nxt[0]-8) if nxt else size
    p=off+start; cgd=struct.unpack_from('>h',rom,p)[0]
    hdr=tuple(rom[p+2:p+8]); out=[];q=p+8;lim=off+end
    if cgd not in (1,2,4,6): return cgd,hdr,None
    while q<lim:
        code=struct.unpack_from('>H',rom,q)[0]
        if code<0x100:
            k,ix,pat=struct.unpack_from('>hhh',rom,q+2); out.append(('C',code,k,ix,pat)); q+=8+max(cgd*4-8,0)
        else:
            se,olc,num=struct.unpack_from('>HHH',rom,q+2)
            r=[code&0xFF,code>>8,se,olc,remap(num)]; q2=q+8
            if cgd>=4:
                att,hit=struct.unpack_from('>hH',rom,q2); e4=tuple(rom[q2+4:q2+8]); r+= [hit,att]+list(e4); q2+=8
            if cgd==6:
                z,riv,ad=struct.unpack_from('>HHH',rom,q2); nx,st=rom[q2+6],rom[q2+7]; r+=[z,riv,ad,nx,st]; q2+=8
            out.append(('L',tuple(r))); q=q2
    return cgd,hdr,out
# ---------- ps2 ----------
ps2=open('ps2_ryu_chd.bin','rb').read(); PS2SZ=len(ps2); N=25
offs=list(struct.unpack_from('<25I',ps2,0))
SECI={'nmca':0,'dmca':1,'btca':2,'caca':3,'cuca':4,'atca':5,'saca':6,'exca':7,'cbca':8,'yuca':9,
 'stxy':10,'mvxy':11,'sernd':12,'ovct':13,'ovix':14,'rict':15,'hiit':16,'boda':17,'hana':18,
 'cata':19,'caua':20,'atta':21,'hosa':22,'atit':23,'prot':24}
def p_span(sec):
    start=offs[sec]; end=PS2SZ
    for i in range(N):
        if offs[i]>start and offs[i]<end: end=offs[i]
    return start,end-start
P={}
for k in ARC:
    b,s=p_span(SECI[k]); e=[];p=b
    while True:
        v=struct.unpack_from('<I',ps2,p)[0];p+=4
        if v==0:break
        e.append(v)
    P[k]=(b,s,e)
def p_parse(name,idx):
    b,size,ents=P[name]; so=sorted(set(ents))
    start=ents[idx]-8; nxt=[o for o in so if o>ents[idx]]
    end=(nxt[0]-8) if nxt else size
    p=b+start; cgd=struct.unpack_from('<h',ps2,p)[0]
    hdr=tuple(ps2[p+2:p+8]); out=[];q=p+8;lim=b+end
    if cgd not in (1,2,4,6): return cgd,hdr,None
    while q<lim:
        code=struct.unpack_from('<H',ps2,q)[0]
        if code<0x100:
            k,ix,pat=struct.unpack_from('<hhh',ps2,q+2); out.append(('C',code,k,ix,pat)); q+=8+max(cgd*4-8,0)
        else:
            se,olc,num=struct.unpack_from('<HHH',ps2,q+2)
            r=[code&0xFF,code>>8,se,olc,num]; q2=q+8
            if cgd>=4:
                hit,att=struct.unpack_from('<Hh',ps2,q2); e4=tuple(ps2[q2+4:q2+8]); r+=[hit,att]+list(e4); q2+=8
            if cgd==6:
                z,riv,ad=struct.unpack_from('<HHH',ps2,q2); nx,st=ps2[q2+6],ps2[q2+7]; r+=[z,riv,ad,nx,st]; q2+=8
            out.append(('L',tuple(r))); q=q2
    return cgd,hdr,out
print("table   arcade_entries ps2_entries  arcade_size ps2_size")
for k in ARC:
    print("%-6s %6d %6d      0x%-6X 0x%-6X"%(k,len(A[k]),len(P[k][2]),ARC[k][1],P[k][2] and P[k][1]))
print()
diffs=0
for k in ARC:
    n=min(len(A[k]),len(P[k][2]))
    for i in range(n):
        ac,ah,al=a_parse(k,i); pc,ph,pl=p_parse(k,i)
        if ac!=pc or ah!=ph or al!=pl:
            diffs+=1
            if diffs<=25:
                print("DIFF %s[%d]: cgd %s/%s hdr %s/%s"%(k,i,ac,pc,ah,ph))
                if al is not None and pl is not None:
                    if len(al)!=len(pl): print("   len %d vs %d"%(len(al),len(pl)))
                    for j in range(min(len(al),len(pl))):
                        if al[j]!=pl[j]: print("   cell %d ARC %s  PS2 %s"%(j,al[j],pl[j]))
print("TOTAL differing scripts:",diffs)
