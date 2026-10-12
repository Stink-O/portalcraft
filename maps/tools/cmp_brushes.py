import sys, collections
from bsp2vmf import *
a=Bsp(sys.argv[1]); b=Bsp(sys.argv[2])
def key(bsp,bi):
    br=bsp.brushes[bi]
    return tuple(sorted((round(bsp.planes[s['planenum']][0][0],3),round(bsp.planes[s['planenum']][0][1],3),round(bsp.planes[s['planenum']][0][2],3),round(bsp.planes[s['planenum']][1],1)) for s in bsp.brushsides[br['firstside']:br['firstside']+br['numsides']] if not s['bevel']))
kb=collections.defaultdict(list)
for bi in range(len(b.brushes)): kb[key(b,bi)].append(bi)
da=Decompiler(a,None)
mis=collections.Counter(); ex=[]
for bi in range(len(a.brushes)):
    k=key(a,bi); c=a.brushes[bi]['contents']
    m=kb.get(k)
    if not m: mis['nomatch']+=1; continue
    cb=[b.brushes[x]['contents'] for x in m]
    if c not in cb:
        br=a.brushes[bi]
        mats=collections.Counter(da.material(s['texinfo'])[0] for s in a.brushsides[br['firstside']:br['firstside']+br['numsides']] if not s['bevel'])
        mis[(hex(c),hex(cb[0]),tuple(sorted(mats)))]+=1
for k,v in mis.most_common(): print(v,k)
