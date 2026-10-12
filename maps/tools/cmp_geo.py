import sys, collections
from bsp2vmf import *
a=Bsp(sys.argv[1]); b=Bsp(sys.argv[2])
da=Decompiler(a,None); db=Decompiler(b,None)
def info(bsp,dc,bi):
    ws=dc.brush_windings(bi)
    pts=[p for *_,w in ws for p in w]
    c=mul(pts[0],0)
    for p in pts: c=add(c,p)
    return mul(c,1/len(pts)), sorted((n,d) for _,_,n,d,_ in ws), len(ws)
from collections import defaultdict
grid=defaultdict(list)
IB=[info(b,db,bi) for bi in range(len(b.brushes))]
for bi,(c,pl,n) in enumerate(IB): grid[tuple(int(x//64) for x in c)].append(bi)
worst=[]; nm=0
for ai in range(len(a.brushes)):
    c,pl,n=info(a,da,ai)
    best=None
    g=tuple(int(x//64) for x in c)
    for dx in (-1,0,1):
      for dy in (-1,0,1):
        for dz in (-1,0,1):
          for bi in grid.get((g[0]+dx,g[1]+dy,g[2]+dz),[]):
            cb,plb,nb=IB[bi]
            if nb!=n: continue
            dev=0
            for (n1,d1) in pl:
                dev=max(dev,min(max(abs(n1[i]-n2[i]) for i in range(3))*1000+abs(d1-d2) for n2,d2 in plb))
            if best is None or dev<best[0]: best=(dev,bi)
    if best is None: nm+=1; worst.append((1e9,ai)); continue
    worst.append((best[0],ai))
worst.sort(reverse=True)
print('no candidate',nm)
print('worst plane deviations (normal*1000 + dist):',[(round(w,4),i) for w,i in worst[:15]])
print('dev>0.01:',sum(1 for w,_ in worst if w>0.01), 'dev>0.5:',sum(1 for w,_ in worst if w>0.5))
