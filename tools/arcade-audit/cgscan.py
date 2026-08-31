import struct,collections
exec(open('scan.py').read().split('issues=[]')[0])
ogt=[int(x) for x in open('ogt.txt').read().split(',')]
# ryu remap
def remap(v):
    if v<0x400: return v
    d=-0x01E0
    if 0x7082<=v<=0x7090: d=-0x62C6
    a=v+d
    if a<0 or a>0xFFFF: return v
    return a
per=collections.defaultdict(list)
allnums=collections.Counter()
for name in RYU:
    for i in range(len(tabs[name])):
        cgd,cells=parse(name,i)
        for c in cells:
            if c[0]=='CELL':
                n=c[1]['num']; allnums[n]+=1
                per[name].append((i,n))
print("distinct raw cg numbers:",len(allnums), "min",hex(min(allnums)),"max",hex(max(allnums)))
grp=collections.Counter()
oob=[]
for n in allnums:
    r=remap(n)
    if r>=len(ogt): oob.append((hex(n),hex(r)))
    else: grp[ogt[r]]+=allnums[n]
print("groups hit:",dict(grp))
print("remap beyond obj_group_table:",oob[:20], len(oob))
# denjin specific
print("--- saca 40 cg numbers ---")
cgd,cells=parse('saca',40)
for c in cells:
    if c[0]=='CELL':
        n=c[1]['num']; r=remap(n)
        print(hex(n),"->",hex(r),"group",ogt[r] if r<len(ogt) else "OOB")
