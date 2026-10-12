import sys, collections, re
from bsp2vmf import *
a=Bsp(sys.argv[1]); b=Bsp(sys.argv[2])
def fkey(bsp,f):
    n,d,_=bsp.planes[f['planenum']]
    return (round(n[0],3),round(n[1],3),round(n[2],3),round(d,0))
idx=collections.defaultdict(list)
for mi,m in enumerate(b.models):
    for fi in range(m['firstface'],m['firstface']+m['numfaces']):
        f=b.faces[fi]; idx[(mi,)+fkey(b,f)].append(fi)
def norm_name(s):
    s=s.lower(); s=re.sub(r'^maps/[^/]+/','',s); s=re.sub(r'(_-?\d+_-?\d+_-?\d+|_wvt_patch)$','',s); return s
stats=collections.Counter(); ex=[]
for mi,m in enumerate(a.models):
    for fi in range(m['firstface'],m['firstface']+m['numfaces']):
        f=a.faces[fi]
        if f['dispinfo']>=0: continue
        w=a.face_winding(f); c=mul(w[0],0)
        for p in w: c=add(c,p)
        c=mul(c,1/len(w)); n=a.planes[f['planenum']][0]
        cand=None
        for gi in idx.get((mi,)+fkey(a,f),[]):
            g=b.faces[gi]; wg=b.face_winding(g)
            if point_in_winding(c,wg,n) or point_in_winding(c,wg[::-1],n): cand=gi; break
        if cand is None: stats['nomatch']+=1; continue
        ta=a.texinfo[f['texinfo']]; tb=b.texinfo[b.faces[cand]['texinfo']]
        na=norm_name(a.texdata[ta['texdata']]['name']); nb=norm_name(b.texdata[tb['texdata']]['name'])
        if na!=nb: stats['material']+=1; ex.append(('mat',na,nb,[tuple(round(x) for x in p) for p in w],[tuple(round(x) for x in p) for p in b.face_winding(b.faces[cand])])); continue
        dv=max(abs(x-y) for k in range(2) for x,y in zip(ta['tv'][k],tb['tv'][k]))
        # shift can differ by texture-size multiples
        if dv>1e-3:
            W=a.texdata[ta['texdata']]['w']; H=a.texdata[ta['texdata']]['h']
            vec_ok=max(abs(x-y) for k in range(2) for x,y in zip(ta['tv'][k][:3],tb['tv'][k][:3]))<1e-4
            sh_ok=all(abs(((ta['tv'][k][3]-tb['tv'][k][3])/(W if k==0 else H)+0.5)%1-0.5)<1e-3 for k in range(2))
            if not (vec_ok and sh_ok): stats['texvec']+=1; ex.append(('tv',na,ta['tv'],tb['tv'])); continue
        la=length(ta['lv'][0][:3]); lb=length(tb['lv'][0][:3])
        if abs(la-lb)>1e-4: stats['lmscale']+=1; ex.append(('lm',na,1/la if la else 0,1/lb if lb else 0)); continue
        if (f['smoothing']!=b.faces[cand]['smoothing']): stats['smooth']+=1
        stats['ok']+=1
print(stats)
for e in ex[:12]: print(e)
