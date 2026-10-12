#!/usr/bin/env python3
"""Carry Valve's baked lighting over to the recompiled chamber.

usage: relight.py <compiled.bsp> <valve_original.bsp> <out.bsp>

Our vrad run lights everything (the hidden region needs it), but its results
for the chamber differ from Valve's 2011 compile (different vrad build,
random ambient sampling, slightly different face splits). Wherever the
recompiled chamber has Valve's geometry, this replaces our baked data with
Valve's:

  - HDR lightmaps (lump 53), per luxel. Lightmap texel grids are world-aligned
    (texinfo lightmap vectors), so a luxel of our face is the same luxel of
    the Valve face on the same plane with the same lightmap vectors, light
    styles and bump layout that covers it. Faces split differently still get
    Valve's texels. Per-style average colours are recomputed.
  - HDR and LDR leaf ambient samples (lumps 51/55, 52/56): a leaf with the
    same bounds as a Valve leaf takes its samples; any other chamber leaf
    keeps its sample positions and takes the cube of the nearest Valve sample
    in the Valve leaf at that point.

Texels and samples with no Valve counterpart (the hidden region, the odd
luxel off the edge of a re-split face) keep our vrad's values. Static prop
vertex lighting is carried over in pack.py. Rewritten lumps are appended at
the end of the file; the pakfile stays last.
"""
import math
import struct
import sys
from collections import defaultdict

from bsp import Bsp

L_LIGHTING_HDR, L_FACES_HDR = 53, 58
L_LEAF_AMB_IDX_HDR, L_LEAF_AMB_IDX = 51, 52
L_LEAF_AMB_HDR, L_LEAF_AMB = 55, 56
L_PAKFILE = 40
FACE_FMT = "<HBBihhhh4Bif2i2iiHHI"


def hdr_faces(b):
    d = b.lump(L_FACES_HDR)
    return [struct.unpack_from(FACE_FMT, d, i * 56) for i in range(len(d) // 56)]


def lit_faces(b):
    """[(model, face index, info dict)] for faces with lightmaps"""
    hf = hdr_faces(b)
    out = []
    for mi, m in enumerate(b.models):
        for fi in range(m["firstface"], m["firstface"] + m["numfaces"]):
            h = hf[fi]
            if h[12] < 0:
                continue
            f = b.faces[fi]
            ti = b.texinfo[f["texinfo"]]
            n, dist, _ = b.planes[f["planenum"]]
            ns = sum(1 for s in h[8:12] if s != 255)
            nb = 4 if ti["flags"] & 0x800 else 1
            w, hh = h[16] + 1, h[17] + 1
            key = (mi, tuple(round(c, 4) for c in n), round(dist, 2),
                   tuple(round(c, 5) for c in ti["lv"][0]), tuple(round(c, 5) for c in ti["lv"][1]),
                   h[8:12], nb)
            out.append((fi, dict(key=key, ofs=h[12], ns=ns, nb=nb, lm=(h[14], h[15]), w=w, h=hh,
                                 n=n, d=dist, lv=ti["lv"], wind=b.face_winding(f))))
    return out


def solve3(r0, r1, r2, b):
    det = (r0[0] * (r1[1] * r2[2] - r1[2] * r2[1]) - r0[1] * (r1[0] * r2[2] - r1[2] * r2[0])
           + r0[2] * (r1[0] * r2[1] - r1[1] * r2[0]))
    if abs(det) < 1e-12:
        return None
    def rep(i):
        rows = [list(r0), list(r1), list(r2)]
        for k in range(3):
            rows[k][i] = b[k]
        a, c, e = rows
        return (a[0] * (c[1] * e[2] - c[2] * e[1]) - a[1] * (c[0] * e[2] - c[2] * e[0])
                + a[2] * (c[0] * e[1] - c[1] * e[0]))
    return tuple(rep(i) / det for i in range(3))


def inside(p, wind, n, tol):
    sgn = 0
    for i in range(len(wind)):
        a, b = wind[i], wind[(i + 1) % len(wind)]
        e = (b[0] - a[0], b[1] - a[1], b[2] - a[2])
        v = (p[0] - a[0], p[1] - a[1], p[2] - a[2])
        c = (e[1] * v[2] - e[2] * v[1], e[2] * v[0] - e[0] * v[2], e[0] * v[1] - e[1] * v[0])
        s = c[0] * n[0] + c[1] * n[1] + c[2] * n[2]
        el = math.sqrt(e[0] ** 2 + e[1] ** 2 + e[2] ** 2) or 1
        s /= el
        if s > tol:
            if sgn < 0:
                return False
            sgn = 1
        elif s < -tol:
            if sgn > 0:
                return False
            sgn = -1
    return True


def rgbe_to_lin(c):
    e = c[3] - 256 if c[3] > 127 else c[3]
    s = 2.0 ** e
    return (c[0] * s, c[1] * s, c[2] * s)


def lin_to_rgbe(v):
    m = max(v)
    if m <= 0:
        return bytes((0, 0, 0, 0))
    e = math.frexp(m)[1] - 8
    s = 2.0 ** -e
    vals = [min(255, max(0, int(x * s + 0.5))) for x in v]
    return bytes(vals + [e & 0xff])


def transplant_lightmaps(comp, orig, data):
    olight = orig.lump(L_LIGHTING_HDR)
    lo = comp.lumps[L_LIGHTING_HDR][0]
    groups = defaultdict(list)
    for fi, o in lit_faces(orig):
        groups[o["key"]].append(o)
    faces = texels = texels_copied = 0
    for fi, c in lit_faces(comp):
        cands = groups.get(c["key"])
        if not cands:
            continue
        w, h, ns, nb = c["w"], c["h"], c["ns"], c["nb"]
        lum = w * h
        copied_any = False
        for j in range(h):
            for i in range(w):
                s, t = c["lm"][0] + i, c["lm"][1] + j
                texels += 1
                p = solve3(c["lv"][0][:3], c["lv"][1][:3], c["n"],
                           (s - c["lv"][0][3], t - c["lv"][1][3], c["d"]))
                best = None
                for o in cands:
                    oi, oj = s - o["lm"][0], t - o["lm"][1]
                    if not (0 <= oi < o["w"] and 0 <= oj < o["h"]):
                        continue
                    if p is not None and inside(p, o["wind"], o["n"], 0.5):
                        best = o
                        break
                    if best is None:
                        best = o
                if best is None:
                    continue
                oi, oj = s - best["lm"][0], t - best["lm"][1]
                olum = best["w"] * best["h"]
                for blk in range(ns * nb):
                    src = best["ofs"] + (blk * olum + oj * best["w"] + oi) * 4
                    dst = lo + c["ofs"] + (blk * lum + j * w + i) * 4
                    data[dst:dst + 4] = olight[src:src + 4]
                texels_copied += 1
                copied_any = True
        if copied_any:
            faces += 1
            # per-style average colour (stored just before the samples, style 0 last)
            for st in range(ns):
                acc = [0.0, 0.0, 0.0]
                base = lo + c["ofs"] + st * nb * lum * 4
                for k in range(lum):
                    v = rgbe_to_lin(data[base + k * 4:base + k * 4 + 4])
                    acc = [acc[q] + v[q] for q in range(3)]
                avg = lin_to_rgbe([x / lum for x in acc])
                pos = lo + c["ofs"] - 4 * (st + 1)
                data[pos:pos + 4] = avg
    print("lightmaps: %d of %d luxels (%d faces) carried from Valve's compile" % (texels_copied, texels, faces))


def leaf_bounds(b):
    d = b.lump(10)
    return [struct.unpack_from("<i", d, i * 32) + struct.unpack_from("<6h", d, i * 32 + 8)
            for i in range(len(d) // 32)]


def ambient_lists(b, idx_lump, samp_lump):
    idx, samp = b.lump(idx_lump), b.lump(samp_lump)
    out = []
    for i in range(len(idx) // 4):
        cnt, first = struct.unpack_from("<HH", idx, i * 4)
        out.append([samp[k * 28:(k + 1) * 28] for k in range(first, first + cnt)])
    return out


def leaf_at(b, p):
    n = 0
    while n >= 0:
        nd = b.nodes[n]
        pn, pd, _ = b.planes[nd["planenum"]]
        n = nd["children"][0] if pn[0] * p[0] + pn[1] * p[1] + pn[2] * p[2] - pd >= 0 else nd["children"][1]
    return -n - 1


def sample_pos(bounds, s):
    mn, mx = bounds[1:4], bounds[4:7]
    return tuple(mn[k] + (mx[k] - mn[k]) * s[24 + k] / 255.0 for k in range(3))


def transplant_ambient(comp, orig):
    ob_leaf = {}
    obounds = leaf_bounds(orig)
    for i, lb in enumerate(obounds):
        ob_leaf.setdefault(lb, i)
    cb = leaf_bounds(comp)
    new_lumps = {}
    for idx_l, samp_l, name in ((L_LEAF_AMB_IDX_HDR, L_LEAF_AMB_HDR, "HDR"),
                                (L_LEAF_AMB_IDX, L_LEAF_AMB, "LDR")):
        if not orig.lump(idx_l) or not comp.lump(idx_l):
            continue
        ol = ambient_lists(orig, idx_l, samp_l)
        cl = ambient_lists(comp, idx_l, samp_l)
        idx_out, samp_out, n = bytearray(), bytearray(), 0
        whole = per_sample = kept = 0
        for i, lst in enumerate(cl):
            j = ob_leaf.get(cb[i])
            if j is not None and j < len(ol) and ol[j]:
                lst = ol[j]
                whole += 1
            elif lst:
                new = []
                for s in lst:
                    p = sample_pos(cb[i], s)
                    oj = leaf_at(orig, p)
                    if orig.leafs[oj]["contents"] & 1 or oj >= len(ol) or not ol[oj]:
                        new.append(s)
                        kept += 1
                        continue
                    best = min(ol[oj], key=lambda q: sum((a - b) ** 2 for a, b in zip(sample_pos(obounds[oj], q), p)))
                    new.append(best[:24] + s[24:])
                    per_sample += 1
                lst = new
            idx_out += struct.pack("<HH", len(lst), n)
            for s in lst:
                samp_out += s
            n += len(lst)
        assert n < 65536, "too many ambient samples"
        new_lumps[idx_l] = bytes(idx_out)
        new_lumps[samp_l] = bytes(samp_out)
        print("%s leaf ambient: %d leaves whole, %d samples from the nearest Valve sample, %d samples kept (region)"
              % (name, whole, per_sample, kept))
    return new_lumps


def main():
    comp_path, orig_path, out_path = sys.argv[1:4]
    comp, orig = Bsp(comp_path), Bsp(orig_path)
    data = bytearray(comp.data)
    transplant_lightmaps(comp, orig, data)
    new_lumps = transplant_ambient(comp, orig)

    po, pl = comp.lumps[L_PAKFILE][0], comp.lumps[L_PAKFILE][1]
    pak = bytes(data[po:po + pl])
    if po + pl == len(data):
        del data[po:]
    for li, blob in new_lumps.items():
        while len(data) % 4:
            data.append(0)
        struct.pack_into("<iii4s", data, 8 + li * 16, len(data), len(blob), comp.lumps[li][2], b"\0\0\0\0")
        data += blob
    while len(data) % 4:
        data.append(0)
    struct.pack_into("<iii4s", data, 8 + L_PAKFILE * 16, len(data), len(pak), 0, b"\0\0\0\0")
    data += pak
    open(out_path, "wb").write(data)


if __name__ == "__main__":
    main()
