import sys, struct, random
from bsp import *
def rgbe(c):
    e=struct.unpack('b',bytes([c[3]]))[0]; s=2.0**e/255
    return [c[i]*s*255 for i in range(3)]
class A:
    def __init__(s,path):
        s.b=Bsp(path); s.idx=s.b.lump(51); s.samp=s.b.lump(55)
        s.leafmm=[struct.unpack_from('<6h',s.b.lump(10),i*32+8) for i in range(len(s.b.leafs))]
    def leaf(s,p):
        b=s.b; n=0
        while n>=0:
            nd=b.nodes[n]; pn,pd,_=b.planes[nd['planenum']]
            n=nd['children'][0] if pn[0]*p[0]+pn[1]*p[1]+pn[2]*p[2]-pd>=0 else nd['children'][1]
        return -n-1
    def avg(s,li):
        cnt,first=struct.unpack_from('<HH',s.idx,li*4)
        if cnt==0: return None
        tot=[0,0,0]
        for k in range(first,first+cnt):
            for f in range(6):
                c=rgbe(s.samp[k*28+4*f:k*28+4*f+4]); tot=[tot[i]+c[i] for i in range(3)]
        return [t/(6*cnt) for t in tot]
a=A(sys.argv[1]); b=A(sys.argv[2])
random.seed(1); rows=[]
w=a.b.models[0]
for _ in range(20000):
    p=[random.uniform(w['mins'][i],w['maxs'][i]) for i in range(3)]
    la=a.leaf(p); lb=b.leaf(p)
    if a.b.leafs[la]['contents']!=0 or b.b.leafs[lb]['contents']!=0: continue
    va=a.avg(la); vb=b.avg(lb)
    if va is None or vb is None: continue
    rows.append((sum(vb)/max(sum(va),1e-6),p,va,vb))
rows.sort()
import statistics
print('points',len(rows),'median ratio',round(statistics.median(r for r,*_ in rows),3),'sum ratio',[round(sum(r[3][i] for r in rows)/sum(r[2][i] for r in rows),3) for i in range(3)])
for r,p,va,vb in rows[-6:]: print(round(r,2),[round(x) for x in p],[round(x,3) for x in va],[round(x,3) for x in vb])
