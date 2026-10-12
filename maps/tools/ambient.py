import sys, struct
from bsp import *
def rgbe(c):
    e=struct.unpack('b',bytes([c[3]]))[0]; s=2.0**e/255
    return tuple(round(c[i]*s*255,2) for i in range(3))
def ambient(path,p):
    b=Bsp(path)
    n=0
    while n>=0:
        nd=b.nodes[n]; pn,pd,_=b.planes[nd['planenum']]
        n=nd['children'][0] if pn[0]*p[0]+pn[1]*p[1]+pn[2]*p[2]-pd>=0 else nd['children'][1]
    li=-n-1
    idx=b.lump(51); samp=b.lump(55)
    cnt,first=struct.unpack_from('<HH',idx,li*4)
    out=[]
    for k in range(first,first+cnt):
        o=k*28; cube=[rgbe(samp[o+4*s:o+4*s+4]) for s in range(6)]
        out.append((samp[o+24:o+27], cube))
    return li,out
for path in sys.argv[2:]:
    p=tuple(map(float,sys.argv[1].split(',')))
    li,out=ambient(path,p)
    print(path.split('/')[-1],'leaf',li,len(out),'samples')
    for pos,c in out[:3]: print('   ',list(pos),c)
