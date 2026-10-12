#!/usr/bin/env python3
"""Structural comparison of two BSPs (original vs recompiled)."""
import sys, collections, struct
from bsp import Bsp, parse_entities
a, b = Bsp(sys.argv[1]), Bsp(sys.argv[2])
print("areas", len(a.areas), len(b.areas), "areaportals", len(a.areaportals), len(b.areaportals))
print("models", len(a.models), len(b.models))
bad = 0
for i, (ma, mb) in enumerate(zip(a.models, b.models)):
    d = max(abs(x - y) for x, y in zip(ma["mins"] + ma["maxs"], mb["mins"] + mb["maxs"]))
    if d > 1.5 and i > 0:
        bad += 1
        if bad < 10: print(" model", i, [round(x) for x in ma["mins"] + ma["maxs"]], [round(x) for x in mb["mins"] + mb["maxs"]])
print("model bound mismatches", bad, "world", [round(x) for x in a.models[0]["mins"]+a.models[0]["maxs"]], [round(x) for x in b.models[0]["mins"]+b.models[0]["maxs"]])
ca = collections.Counter(hex(x["contents"]) for x in a.brushes); cb = collections.Counter(hex(x["contents"]) for x in b.brushes)
print("brush contents", "same" if ca == cb else (ca, cb))
print("faces", len(a.faces), len(b.faces), "leafs", len(a.leafs), len(b.leafs), "cubemaps", len(a.cubemaps), len(b.cubemaps))
ea = parse_entities(a.entities_text); eb = parse_entities(b.entities_text)
print("entities", len(ea), len(eb))
ka = collections.Counter(dict(e)["classname"] for e in ea); kb = collections.Counter(dict(e)["classname"] for e in eb)
print("class diff", {k: (ka[k], kb[k]) for k in set(ka) | set(kb) if ka[k] != kb[k]})
# per hammerid key diffs
ia = {dict(e).get("hammerid"): e for e in ea}; ib = {dict(e).get("hammerid"): e for e in eb}
nd = 0
for h, e in ia.items():
    f = ib.get(h)
    if f is None: continue
    sa = collections.Counter(e); sb = collections.Counter(f)
    diff = [(k, v) for k, v in (sa - sb)] + [("+" + k, v) for k, v in (sb - sa)]
    diff = [x for x in diff if x[0].lstrip("+") not in ("model",)]
    if diff:
        nd += 1
        if nd < 15: print(" ent", dict(e)["classname"], h, diff[:6])
print("entities with key diffs", nd)
