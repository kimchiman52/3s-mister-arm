import struct
ps2=open('ps2_ryu_chd.bin','rb').read(); PS2SZ=len(ps2)
N=25
offs=list(struct.unpack_from('<25I',ps2,0))
SEC=['NMCA','DMCA','BTCA','CACA','CUCA','ATCA','SACA','EXCA','CBCA','YUCA','STXY','MVXY','SERND','OVCT','OVIX','RICT','HIIT','BODA','HANA','CATA','CAUA','ATTA','HOSA','ATIT','PROT']
def span(sec):
    hdr=N*4; start=offs[sec]; end=PS2SZ
    for i in range(N):
        if offs[i]>start and offs[i]<end: end=offs[i]
    return start,end-start
KOCSEC={0:0,1:1,2:3,3:4,4:5,5:6,6:2,7:7,8:8,9:9}  # koc -> section idx (charid.c:87-96)
tabs={}
for koc,sec in KOCSEC.items():
    base,size=span(sec)
    ents=[];p=base
    while True:
        v=struct.unpack_from('<I',ps2,p)[0]; p+=4
        if v==0: break
        ents.append(v)
    tabs[koc]=(base,size,ents)
for koc in sorted(tabs):
    b,s,e=tabs[koc]
    print("PS2 koc=%d %-5s base=0x%X size=%u entries=%d"%(koc,SEC[KOCSEC[koc]],b,s,len(e)))
def parse(koc,idx):
    base,size,ents=tabs[koc]
    so=sorted(set(ents)); start=ents[idx]-8
    nxt=[o for o in so if o>ents[idx]]
    end=(nxt[0]-8) if nxt else size
    p=base+start
    cgd=struct.unpack_from('<h',ps2,p)[0]
    out=[];q=p+8;limit=base+end
    if cgd not in (1,2,4,6): return cgd,[]
    while q<limit:
        code=struct.unpack_from('<H',ps2,q)[0]
        if code<0x100:
            k,ix,pat=struct.unpack_from('<hhh',ps2,q+2)
            out.append(('CMD',code,k,ix,pat)); q+=8+max(cgd*4-8,0)
        else:
            r={'type':code&0xFF,'ctr':code>>8}
            se,olc,num=struct.unpack_from('<HHH',ps2,q+2); r.update(se=se,olc=olc,num=num)
            q2=q+8
            if cgd>=4:
                hit,att=struct.unpack_from('<Hh',ps2,q2)
                ext,canc,eff,eft=struct.unpack_from('<4B',ps2,q2+4)
                r.update(hit=hit,att=att,eff=eff,eftype=eft); q2+=8
            if cgd==6: q2+=8
            out.append(('CELL',r)); q=q2
    return cgd,out
CMDN={1:'roa',2:'end',3:'jmp',4:'jpss',5:'jsr',6:'ret',12:'for',13:'nex',28:'rja7',29:'uja7',43:'exec',61:'stop',69:'back',73:'wset',90:'imgs',114:'ifs2'}
for koc,i,label in [(5,40,'saca[40] Denjin'),(5,41,'saca[41]'),(5,42,'saca[42]'),(8,13,'cbca[13] charge'),(8,14,'cbca[14]'),(8,15,'cbca[15]'),(8,16,'cbca[16]'),(8,17,'cbca[17]'),(8,18,'cbca[18]'),(8,26,'cbca[26]'),(5,36,'saca[36] Shinkuu')]:
    cgd,cells=parse(koc,i)
    print("=== PS2 %s  cgd=%d cells=%d"%(label,cgd,len(cells)))
    for k,c in enumerate(cells):
        if c[0]=='CMD': print("  %2d %s(%d) koc=%d(0x%x) ix=%d pat=%d"%(k,CMDN.get(c[1],'?%d'%c[1]),c[1],c[2],c[2]&0xffff,c[3],c[4]))
        else:
            r=c[1]; print("  %2d CELL type=%d ctr=%d se=0x%04x olc=0x%04x num=0x%04x eff=%s eftype=%s att=%s hit=%s"%(k,r['type'],r['ctr'],r['se'],r['olc'],r['num'],r.get('eff'),r.get('eftype'),r.get('att'),r.get('hit')))
