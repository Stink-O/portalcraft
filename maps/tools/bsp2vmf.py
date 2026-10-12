#!/usr/bin/env python3
"""Decompile a Portal 2 BSP (v21) back into a VMF that vbsp can recompile.

usage: bsp2vmf.py <in.bsp> <out.vmf> [--map-name NAME] [--report out.json]

What is reconstructed, and from where:
  brushes        brush + brushside + plane lumps; each side's polygon is the
                 brush's plane intersection (exact to float precision), so
                 collision and portalability match. Bevel sides are dropped.
  world / detail world model brushes; CONTENTS_DETAIL brushes -> func_detail
  brush entities model *N brushes, moved back from model space to world space
                 with the entity's origin and angles
  areaportals    the stray CONTENTS_AREAPORTAL brushes -> func_areaportal(window)
                 matched by clip-portal geometry
  textures       texinfo -> material, uaxis/vaxis/scale/shift, lightmapscale;
                 cubemap / WVT patched materials are unpatched via the pakfile
  displacements  dispinfo + dispverts -> dispinfo blocks on the base side
  overlays       info_overlay_accessor entities (Valve's own keyvalues) and the
                 overlay lump for the rest; faces -> side ids via the face-id lump
  static props   sprp game lump -> prop_static entities
  cubemaps       cubemap lump -> env_cubemap, with explicit side lists taken from
                 the patched materials so the assignment is identical
  entities       everything else verbatim (I/O kept as connections)
"""
import argparse
import json
import math
import re
import struct
import sys
from collections import defaultdict

from bsp import Bsp, parse_entities, CONTENTS_DETAIL

AREAPORTAL = 0x8000
EPS = 1e-3


# ---------------------------------------------------------------- vector math
def dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def add(a, b):
    return (a[0] + b[0], a[1] + b[1], a[2] + b[2])


def mul(a, s):
    return (a[0] * s, a[1] * s, a[2] * s)


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def length(a):
    return math.sqrt(dot(a, a))


def norm(a):
    l = length(a)
    return mul(a, 1.0 / l) if l > 0 else a


def angle_matrix(ang):
    """Source AngleMatrix: rows of the 3x3 (world = M @ local)."""
    p, y, r = (math.radians(a) for a in ang)
    sp, cp = math.sin(p), math.cos(p)
    sy, cy = math.sin(y), math.cos(y)
    sr, cr = math.sin(r), math.cos(r)
    fwd = (cp * cy, cp * sy, -sp)
    left = (sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, sr * cp)
    up = (cr * sp * cy + sr * sy, cr * sp * sy - sr * cy, cr * cp)
    return [(fwd[i], left[i], up[i]) for i in range(3)]


def mat_vec(m, v):
    return tuple(dot(m[i], v) for i in range(3))


# ---------------------------------------------------------------- windings
def base_winding(n, d, size=65536.0):
    ax = max(range(3), key=lambda i: abs(n[i]))
    up = (0, 0, 1) if ax != 2 else (1, 0, 0)
    v = norm(sub(up, mul(n, dot(up, n))))
    r = cross(v, n)
    org = mul(n, d)
    v = mul(v, size)
    r = mul(r, size)
    return [add(sub(org, v), r), add(add(org, v), r), sub(add(org, v), r), sub(sub(org, v), r)]


def clip_winding(w, n, d, eps=0.01):
    """Keep the part of w behind plane (n, d): n.p <= d."""
    out = []
    dists = [dot(p, n) - d for p in w]
    for i, p in enumerate(w):
        q = w[(i + 1) % len(w)]
        dp, dq = dists[i], dists[(i + 1) % len(w)]
        if dp <= eps:
            out.append(p)
        if (dp > eps and dq < -eps) or (dp < -eps and dq > eps):
            t = dp / (dp - dq)
            out.append(add(p, mul(sub(q, p), t)))
    return out


def snap(v, eps=0.001):
    r = round(v)
    return float(r) if abs(v - r) < eps else round(v, 6)


def fmt_num(v):
    if abs(v) < 1e-9:
        v = 0.0
    s = ("%.6f" % v).rstrip("0").rstrip(".")
    return "0" if s in ("-0", "") else s


def fmt_vec(v):
    return " ".join(fmt_num(x) for x in v)


def point_in_winding(pt, w, n, eps=0.1):
    for i in range(len(w)):
        a, b = w[i], w[(i + 1) % len(w)]
        edge_n = cross(n, sub(b, a))  # points inward for clockwise-from-front
        # accept either orientation: compute sign consistency later
        if dot(sub(pt, a), edge_n) < -eps:
            return False
    return True


def winding_area(w):
    s = (0.0, 0.0, 0.0)
    for i in range(1, len(w) - 1):
        s = add(s, cross(sub(w[i], w[0]), sub(w[i + 1], w[0])))
    return length(s) * 0.5


# ---------------------------------------------------------------- VMF writer
class Out:
    def __init__(self):
        self.lines = []
        self.depth = 0

    def open(self, name):
        self.lines.append("\t" * self.depth + name)
        self.lines.append("\t" * self.depth + "{")
        self.depth += 1

    def close(self):
        self.depth -= 1
        self.lines.append("\t" * self.depth + "}")

    def kv(self, k, v):
        self.lines.append("\t" * self.depth + '"%s" "%s"' % (k, v))

    def text(self):
        return "\n".join(self.lines) + "\n"


# ---------------------------------------------------------------- decompiler
class Decompiler:
    def __init__(self, bsp, map_name):
        self.b = bsp
        self.map_name = map_name
        self.next_id = 1
        self.report = defaultdict(int)
        self.notes = []
        self.unpatch = self._load_unpatch()
        self.si_of_face = {}        # bsp face index -> brush side index
        self.si_by_hammer = {}      # Valve hammer side id -> brush side index
        self.sid_of_si = {}         # brush side index -> vmf side id
        self.smoothing = {}
        self.cubemap_si = defaultdict(set)
        self.windings = {}
        self.side_override = {}     # brush side index -> texinfo to use instead

    def nid(self):
        i = self.next_id
        self.next_id += 1
        return i

    # ----- materials
    def _load_unpatch(self):
        out = {}
        try:
            z = self.b.pakfile()
        except Exception:
            return out
        for n in z.namelist():
            if not n.lower().endswith(".vmt"):
                continue
            txt = z.read(n).decode("latin-1")
            m = re.search(r'"?include"?\s+"([^"]+)"', txt, re.I)
            if m:
                inc = m.group(1).replace("\\", "/")
                inc = re.sub(r"^materials/", "", inc, flags=re.I)
                inc = re.sub(r"\.vmt$", "", inc, flags=re.I)
                key = re.sub(r"^materials/", "", n, flags=re.I)[:-4].lower()
                out[key] = inc
        return out

    def material(self, texinfo_idx):
        """-> (material name, cubemap origin or None)"""
        if texinfo_idx < 0:
            return "TOOLS/TOOLSNODRAW", None
        name = self.b.texdata[self.b.texinfo[texinfo_idx]["texdata"]]["name"]
        low = name.lower().replace("\\", "/")
        cube = None
        if low in self.unpatch:
            m = re.search(r"_(-?\d+)_(-?\d+)_(-?\d+)$", low)
            if m and "_wvt_patch" not in low:
                cube = tuple(int(x) for x in m.groups())
            name = self.unpatch[low]
        elif low.startswith("maps/"):
            # patched material whose vmt is missing: strip the prefix and suffix
            m = re.match(r"maps/[^/]+/(.*?)(?:_(-?\d+)_(-?\d+)_(-?\d+)|_wvt_patch)?$", low)
            if m:
                name = m.group(1)
                if m.group(2) is not None:
                    cube = (int(m.group(2)), int(m.group(3)), int(m.group(4)))
            if "_wvt_patch" not in low:
                self.notes.append("unpatched without vmt: " + low)
        return name.upper(), cube

    # ----- model brushes
    def model_brushes(self, m):
        b = self.b
        st = [b.models[m]["headnode"]]
        out = set()
        while st:
            n = st.pop()
            if n < 0:
                l = b.leafs[-n - 1]
                out.update(b.leafbrushes[l["firstleafbrush"]:l["firstleafbrush"] + l["numleafbrushes"]])
            else:
                st.extend(b.nodes[n]["children"])
        return sorted(out)

    def brush_windings(self, bi):
        b = self.b
        br = b.brushes[bi]
        sides = []
        for k in range(br["numsides"]):
            si = br["firstside"] + k
            s = b.brushsides[si]
            if s["bevel"]:
                continue
            n, d, _ = b.planes[s["planenum"]]
            sides.append((si, s, n, d))
        out = []
        for i, (si, s, n, d) in enumerate(sides):
            w = base_winding(n, d)
            for j, (_, _, n2, d2) in enumerate(sides):
                if i == j:
                    continue
                if dot(n, n2) > 0.99999 and abs(d - d2) < 0.01:
                    continue  # duplicate plane
                w = clip_winding(w, n2, d2)
                if len(w) < 3:
                    break
            if len(w) >= 3 and winding_area(w) > 0.01:
                out.append((si, s, n, d, w))
        return out

    # ----- face -> brush side (model space, before anything is emitted)
    def map_faces(self, model_idx, brushes):
        b = self.b
        m = b.models[model_idx]
        by_plane = defaultdict(list)
        for bi in brushes:
            for si, s, n, d, w in self.windings[bi]:
                key = (round(n[0], 3), round(n[1], 3), round(n[2], 3), round(d, 1))
                by_plane[key].append((si, n, w, s["texinfo"]))
        for fi in range(m["firstface"], m["firstface"] + m["numfaces"]):
            f = b.faces[fi]
            n, d, _ = b.planes[f["planenum"]]
            key = (round(n[0], 3), round(n[1], 3), round(n[2], 3), round(d, 1))
            fw = b.face_winding(f)
            c = (0.0, 0.0, 0.0)
            for p in fw:
                c = add(c, p)
            c = mul(c, 1.0 / len(fw))
            best = None
            for si, sn, w, ti in by_plane.get(key, []):
                if point_in_winding(c, w, sn) or point_in_winding(c, w[::-1], sn):
                    if best is None or ti == f["texinfo"]:
                        best = si
            if best is None:
                self.report["faces_unmapped"] += 1
                continue
            self.si_of_face[fi] = best
            self.smoothing[best] = self.smoothing.get(best, 0) | f["smoothing"]
            hid = b.faceids[fi] if fi < len(b.faceids) else 0
            if hid:
                self.si_by_hammer.setdefault(hid, best)
            _, cube = self.material(f["texinfo"])
            if cube is not None:
                self.cubemap_si[cube].add(best)
        self.resolve_coplanar(model_idx, brushes, by_plane)

    def resolve_coplanar(self, model_idx, brushes, by_plane):
        """Two brushes with overlapping coplanar sides: which one's face
        survives depends on vbsp's node splits, which the hidden region
        changes. Where a side made no face in Valve's compile but its centre
        lies inside a Valve face from another brush on the same plane, give
        the side that face's surface (material and alignment), so either
        outcome looks and behaves like Valve's."""
        b = self.b
        m = b.models[model_idx]
        with_face = set(self.si_of_face.values())
        brush_of = {}
        for bi in brushes:
            for si, *_ in self.windings[bi]:
                brush_of[si] = bi
        faces_by_plane = defaultdict(list)
        for fi in range(m["firstface"], m["firstface"] + m["numfaces"]):
            src = self.si_of_face.get(fi)
            if src is None:
                continue
            f = b.faces[fi]
            n, d, _ = b.planes[f["planenum"]]
            key = (round(n[0], 3), round(n[1], 3), round(n[2], 3), round(d, 1))
            faces_by_plane[key].append((src, n, b.face_winding(f), f["texinfo"]))
        for key, sides in by_plane.items():
            for si, sn, w, ti in sides:
                if si in with_face or si in self.side_override:
                    continue
                c = (0.0, 0.0, 0.0)
                for p in w:
                    c = add(c, p)
                c = mul(c, 1.0 / len(w))
                for src, fn, fw, fti in faces_by_plane.get(key, []):
                    if brush_of.get(src) == brush_of.get(si):
                        continue
                    if point_in_winding(c, fw, fn, eps=-0.5) or point_in_winding(c, fw[::-1], fn, eps=-0.5):
                        if fti != ti:
                            self.side_override[si] = fti
                            self.report["coplanar_sides_matched"] += 1
                        break

    # ----- solids
    def emit_solid(self, out, bi, xform=None, disp_by_face=None):
        """xform: (matrix, origin) model->world. Returns number of sides."""
        b = self.b
        ws = self.windings[bi] if isinstance(bi, int) else bi
        if len(ws) < 4:
            self.report["brushes_degenerate"] += 1
            return 0
        out.open("solid")
        out.kv("id", self.nid())
        for si, s, n, d, w in ws:
            sid = self.nid()
            self.sid_of_si[si] = sid
            ti = self.side_override.get(si, s["texinfo"])
            mat, _ = self.material(ti)
            if isinstance(bi, int) and si not in self.side_override:
                mat = self.fix_tool_material(mat, b.brushes[bi]["contents"])
            wn = n
            ww = w
            if xform:
                M, org = xform
                ww = [add(mat_vec(M, p), org) for p in w]
                wn = mat_vec(M, n)
            # choose three well separated points, oriented so the plane faces out
            pts = [tuple(snap(c) for c in p) for p in ww]
            p0 = pts[0]
            p1 = max(pts, key=lambda p: length(sub(p, p0)))
            p2 = max(pts, key=lambda p: length(cross(sub(p1, p0), sub(p, p0))))
            nrm = cross(sub(p0, p1), sub(p2, p1))
            if dot(nrm, wn) < 0:
                p1, p2 = p2, p1
            out.open("side")
            out.kv("id", sid)
            out.kv("plane", "(%s) (%s) (%s)" % (fmt_vec(p0), fmt_vec(p1), fmt_vec(p2)))
            out.kv("material", mat)
            ua, va, lscale = self.tex_axes(ti, xform)
            out.kv("uaxis", ua)
            out.kv("vaxis", va)
            out.kv("rotation", "0")
            out.kv("lightmapscale", lscale)
            out.kv("smoothing_groups", str(self.smoothing.get(si, 0)))
            if disp_by_face and si in disp_by_face:
                self.emit_disp(out, disp_by_face[si])
            out.close()
        out.close()
        self.report["brushes"] += 1
        return len(ws)

    def fix_tool_material(self, mat, contents):
        """vbsp compacts texinfos that no face uses and remaps the brush sides
        that pointed at them (here: onto TOOLSPLAYERCLIP). Recover a material
        that yields the brush's recorded contents."""
        if mat != "TOOLS/TOOLSPLAYERCLIP":
            return mat
        if contents & 0x30000 == 0x30000:
            self.report["tool_material_fixed"] += 1
            return "TOOLS/TOOLSCLIP"
        if contents & 0x10000:
            return mat
        self.report["tool_material_fixed"] += 1
        if contents & 0x80:
            return "TOOLS/TOOLSBLOCKLIGHT"
        if contents & 0x40:
            return "TOOLS/TOOLSBLOCK_LOS"
        return "TOOLS/TOOLSNODRAW"

    def tex_axes(self, ti, xform):
        if ti < 0:
            return "[1 0 0 0] 0.25", "[0 -1 0 0] 0.25", "16"
        t = self.b.texinfo[ti]
        res = []
        for k in range(2):
            v = t["tv"][k]
            a = tuple(v[:3])
            shift = v[3]
            if xform:
                M, org = xform
                a = mat_vec(M, a)
                shift = shift - dot(a, org)
            l = length(a)
            if l < 1e-9:
                res.append("[%s 0] 0.25" % ("1 0 0" if k == 0 else "0 -1 0"))
                continue
            res.append("[%s %s] %s" % (fmt_vec(mul(a, 1 / l)), fmt_num(shift), fmt_num(1 / l)))
        lv = self.b.texinfo[ti]["lv"][0]
        ll = length(lv[:3])
        ls = str(int(round(1 / ll))) if ll > 1e-9 else "16"
        return res[0], res[1], ls

    # ----- displacements
    def emit_disp(self, out, di):
        b = self.b
        d = b.dispinfo[di]
        p = d["power"]
        n = (1 << p) + 1
        face = b.faces[d["mapface"]]
        pn, _, _ = b.planes[face["planenum"]]
        verts = b.dispverts[d["vertstart"]:d["vertstart"] + n * n]
        out.open("dispinfo")
        out.kv("power", p)
        out.kv("startposition", "[%s]" % fmt_vec(d["start"]))
        out.kv("flags", "0")
        out.kv("elevation", "0")
        out.kv("subdiv", "0")
        rows = lambda f: [" ".join(f(verts[r * n + c]) for c in range(n)) for r in range(n)]
        for name, f in (("normals", lambda v: fmt_vec(v[0])),
                        ("distances", lambda v: fmt_num(v[1])),
                        ("offsets", lambda v: "0 0 0"),
                        ("offset_normals", lambda v: fmt_vec(pn)),
                        ("alphas", lambda v: fmt_num(v[2]))):
            out.open(name)
            for r, row in enumerate(rows(f)):
                out.kv("row%d" % r, row)
            out.close()
        nt = 1 << p
        tris = b.disptris[d["tristart"]:d["tristart"] + 2 * nt * nt]
        out.open("triangle_tags")
        for r in range(nt):
            out.kv("row%d" % r, " ".join(str(tris[r * 2 * nt + c]) for c in range(2 * nt)))
        out.close()
        out.open("allowed_verts")
        out.kv("10", " ".join(str(struct.unpack("<i", struct.pack("<I", a))[0]) for a in d["allowed"]))
        out.close()
        out.close()
        self.report["displacements"] += 1

    # ----- main
    def run(self):
        b = self.b
        ents = parse_entities(b.entities_text)
        world = ents[0]
        # smoothing groups per brush side, via face -> side after mapping (second pass
        # is unnecessary for a first cut: faces carry the side's smoothing group)

        # displacement faces -> brush side index (resolved after windings)
        world_brushes = self.model_brushes(0)
        all_used = set(world_brushes)
        model_of_entity = {}
        for ei, e in enumerate(ents):
            d = dict(e)
            if d.get("model", "").startswith("*"):
                model_of_entity[ei] = int(d["model"][1:])
        model_brushes = {m: self.model_brushes(m) for m in set(model_of_entity.values())}
        for v in model_brushes.values():
            all_used.update(v)
        stray = [i for i in range(len(b.brushes)) if i not in all_used]
        areaportal_brushes = [i for i in stray if b.brushes[i]["contents"] & AREAPORTAL]
        stray_other = [i for i in stray if not b.brushes[i]["contents"] & AREAPORTAL]
        self.report["stray_brushes_to_world"] = len(stray_other)
        world_brushes = sorted(set(world_brushes) | set(stray_other))

        for bi in range(len(b.brushes)):
            self.windings[bi] = self.brush_windings(bi)
        self.map_faces(0, world_brushes)
        for m, bl in model_brushes.items():
            self.map_faces(m, bl)

        # displacements: vbsp drops the brush a displacement was built on, so
        # rebuild a thin brush under each base face (top = the displacement side,
        # the rest nodraw; displacement brushes are not part of the solid BSP).
        disp_solids = []
        disp_side = {}
        for di, dd in enumerate(b.dispinfo):
            f = b.faces[dd["mapface"]]
            n, dist, _ = b.planes[f["planenum"]]
            fw = b.face_winding(f)
            planes = [(n, dist, f["texinfo"], ("disp", di))]
            planes.append((mul(n, -1), -(dist - 8.0), -1, ("disp", di, "b")))
            for k in range(len(fw)):
                p, q = fw[k], fw[(k + 1) % len(fw)]
                en = norm(cross(sub(q, p), n))
                c = mul(add(add(fw[0], fw[1]), add(fw[2], fw[3])), 0.25)
                if dot(en, sub(p, c)) < 0:
                    en = mul(en, -1)
                planes.append((en, dot(en, p), -1, ("disp", di, k)))
            ws = []
            for i, (pn, pd, ti, key) in enumerate(planes):
                w = base_winding(pn, pd)
                for j, (pn2, pd2, _, _) in enumerate(planes):
                    if i != j:
                        w = clip_winding(w, pn2, pd2)
                ws.append((key, {"texinfo": ti}, pn, pd, w))
            disp_side[("disp", di)] = di
            disp_solids.append(ws)

        # smoothing groups from faces (brush side via plane match later); approximate
        # by the side's own texinfo faces: filled in map_faces pass below
        out = Out()
        out.open("versioninfo")
        for k, v in (("editorversion", "400"), ("editorbuild", "6412"), ("mapversion", "1"),
                     ("formatversion", "100"), ("prefab", "0")):
            out.kv(k, v)
        out.close()
        out.open("visgroups")
        out.close()
        out.open("viewsettings")
        out.kv("bSnapToGrid", "1")
        out.kv("bShowGrid", "1")
        out.kv("bShowLogicalGrid", "0")
        out.kv("nGridSpacing", "64")
        out.kv("bShow3DGrid", "0")
        out.close()

        # pre-pass: smoothing groups per brush side from its faces (same plane+texinfo)
        # (simple: faces carry smoothing; map by plane & containment is done in map_faces)

        # ---- world
        out.open("world")
        out.kv("id", self.nid())
        skip_world_keys = {"world_mins", "world_maxs", "hammerid"}
        for k, v in world:
            if k not in skip_world_keys:
                out.kv(k, v)
        detail_brushes = []
        for bi in world_brushes:
            c = b.brushes[bi]["contents"]
            if c & CONTENTS_DETAIL:
                detail_brushes.append(bi)
                continue
            self.emit_solid(out, bi)
        for ws in disp_solids:
            self.emit_solid(out, ws, disp_by_face=disp_side)
        out.close()
        world_text_end = len(out.lines)

        # ---- func_detail: one entity per brush keeps things simple
        for bi in detail_brushes:
            out.open("entity")
            out.kv("id", self.nid())
            out.kv("classname", "func_detail")
            self.emit_solid(out, bi)
            out.close()
            self.report["func_detail"] += 1

        # ---- overlays: accessor entities carry Valve's keyvalues
        accessor_ids = set()
        overlay_ents = []
        for e in ents:
            d = dict(e)
            if d["classname"] == "info_overlay_accessor":
                accessor_ids.add(int(d["OverlayID"]))

        # ---- entities
        ap_geo = self.areaportal_geometry(areaportal_brushes)
        ents_out = []
        for ei, e in enumerate(ents[1:], start=1):
            d = dict(e)
            cls = d["classname"]
            eid = d.get("hammerid")
            kvs = []
            conns = []
            for k, v in e:
                if k in ("hammerid",):
                    continue
                if ei in model_of_entity and k == "model":
                    continue
                if cls.startswith("func_areaportal") and k.lower() in ("portalnumber", "origin"):
                    # vbsp would move the brush into origin-local space; the
                    # areaportal brush must stay in world space
                    continue
                if cls == "info_overlay_accessor" and k == "OverlayID":
                    continue
                if "\x1b" in v or (re.match(r"^On[A-Z]|^Out", k) and v.count(",") >= 4):
                    conns.append((k, v))
                else:
                    kvs.append((k, v))
            if cls == "info_overlay_accessor":
                kvs = [(k, "info_overlay" if k == "classname" else v) for k, v in kvs]
                kvs = [(k, self.remap_sides(v) if k == "sides" else v) for k, v in kvs]
            ents_out.append((ei, cls, eid, kvs, conns, d))

        for ei, cls, eid, kvs, conns, d in ents_out:
            out.open("entity")
            out.kv("id", eid if eid else self.nid())
            for k, v in kvs:
                out.kv(k, v)
            if conns:
                out.open("connections")
                for k, v in conns:
                    out.kv(k, v)
                out.close()
            if ei in model_of_entity:
                m = model_of_entity[ei]
                org = tuple(float(x) for x in d.get("origin", "0 0 0").split())
                # vbsp only translates brush-entity brushes by the origin; the
                # engine applies "angles" to the model at runtime, so the model
                # stays unrotated here (rotating it would double the rotation)
                M = angle_matrix((0.0, 0.0, 0.0))
                for bi in model_brushes[m]:
                    self.emit_solid(out, bi, xform=(M, org))
                self.report["brush_entities"] += 1
            if cls.startswith("func_areaportal"):
                pn = int(d.get("portalnumber", d.get("PortalNumber", "-1")))
                bi = ap_geo.get(pn)
                if bi is None:
                    self.notes.append("areaportal %s: no brush" % pn)
                else:
                    self.emit_solid(out, bi)
                    self.report["areaportals"] += 1
            out.close()

        # ---- overlays not exposed as accessors
        self.emit_overlays(out, accessor_ids)
        # ---- static props
        self.emit_static_props(out)
        # ---- cubemaps
        self.emit_cubemaps(out)
        # re-resolve overlay side placeholders now that every face is mapped
        text = out.text()
        text = re.sub(r"@@SIDES:([0-9 ]*)@@", lambda m: self.remap_sides(m.group(1), faces=True), text)
        text = re.sub(r"@@HSIDES:([0-9 ]*)@@", lambda m: self.remap_sides(m.group(1)), text)
        return text

    def remap_sides(self, v, faces=False):
        # deferred: hammer ids are only fully known after all models are mapped
        if not getattr(self, "_mapping_done", False):
            return "@@%s:%s@@" % ("SIDES" if faces else "HSIDES", v)
        ids = []
        for tok in v.split():
            x = int(tok)
            si = self.si_of_face.get(x) if faces else self.si_by_hammer.get(x)
            s = self.sid_of_si.get(si)
            if s is None:
                self.report["overlay_sides_unmapped"] += 1
            elif s not in ids:
                ids.append(s)
        return " ".join(str(i) for i in ids)

    def areaportal_geometry(self, brushes):
        """portal key -> brush index, by matching the clip-portal winding."""
        b = self.b
        out = {}
        for ap in b.areaportals[1:]:
            vs = b.clipportalverts[ap["firstvert"]:ap["firstvert"] + ap["numverts"]]
            if not vs:
                continue
            c = mul(vs[0], 0)
            for v in vs:
                c = add(c, v)
            c = mul(c, 1.0 / len(vs))
            for bi in brushes:
                ws = self.windings[bi]
                inside = all(dot(c, n) - d <= 0.5 for _, _, n, d, _ in ws)
                if inside:
                    out[ap["key"]] = bi
        return out

    def emit_overlays(self, out, skip):
        b = self.b
        from bsp import L_OVERLAYS, L_OVERLAY_FADES, L_OVERLAY_SYSTEM_LEVELS
        lump = b.lump(L_OVERLAYS)
        fades = b.lump(L_OVERLAY_FADES)
        levels = b.lump(L_OVERLAY_SYSTEM_LEVELS)
        for k in range(len(lump) // 352):
            if k in skip:
                continue
            o = k * 352
            oid, ti, fc = struct.unpack_from("<ihH", lump, o)
            count = fc & 0x3fff
            order = fc >> 14
            faces = struct.unpack_from("<%di" % count, lump, o + 8)
            u0, u1, v0, v1 = struct.unpack_from("<4f", lump, o + 264)
            pts = [struct.unpack_from("<3f", lump, o + 280 + 12 * i) for i in range(4)]
            origin = struct.unpack_from("<3f", lump, o + 328)
            normal = struct.unpack_from("<3f", lump, o + 340)
            bu = (pts[0][2], pts[1][2], pts[2][2])
            bv = cross(normal, bu)
            mat, _ = self.material(ti)
            out.open("entity")
            out.kv("id", self.nid())
            out.kv("classname", "info_overlay")
            out.kv("material", mat)
            out.kv("sides", "@@SIDES:%s@@" % " ".join(str(f) for f in faces))
            out.kv("RenderOrder", order)
            out.kv("StartU", fmt_num(u0))
            out.kv("EndU", fmt_num(u1))
            out.kv("StartV", fmt_num(v0))
            out.kv("EndV", fmt_num(v1))
            out.kv("BasisOrigin", fmt_vec(origin))
            out.kv("BasisU", fmt_vec(bu))
            out.kv("BasisV", fmt_vec(bv))
            out.kv("BasisNormal", fmt_vec(normal))
            for i in range(4):
                out.kv("uv%d" % i, "%s %s 0" % (fmt_num(pts[i][0]), fmt_num(pts[i][1])))
            out.kv("origin", fmt_vec(origin))
            if len(fades) >= (k + 1) * 8:
                fmin, fmax = struct.unpack_from("<2f", fades, k * 8)
                out.kv("fademindist", fmt_num(math.sqrt(fmin)) if fmin > 0 else "-1")
                out.kv("fademaxdist", fmt_num(math.sqrt(fmax)) if fmax > 0 else "0")
            if len(levels) >= (k + 1) * 4:
                a, bb, c, dd = struct.unpack_from("<4B", levels, k * 4)
                out.kv("mincpulevel", a)
                out.kv("maxcpulevel", bb)
                out.kv("mingpulevel", c)
                out.kv("maxgpulevel", dd)
            out.close()
            self.report["overlays_from_lump"] += 1

    def emit_static_props(self, out):
        gl = self.b.game_lump()
        if "sprp" not in gl:
            return
        ver, d = gl["sprp"]
        n, = struct.unpack_from("<i", d, 0)
        names = [d[4 + i * 128:4 + i * 128 + 128].split(b"\0")[0].decode() for i in range(n)]
        o = 4 + n * 128
        nl, = struct.unpack_from("<i", d, o)
        o += 4 + nl * 2
        npr, = struct.unpack_from("<i", d, o)
        o += 4
        size = (len(d) - o) // npr if npr else 0
        assert ver in (9, 10) and size in (72, 76), (ver, size)
        self.lighting_origins = []
        for k in range(npr):
            r = d[o + k * size:o + (k + 1) * size]
            org = struct.unpack_from("<3f", r, 0)
            ang = struct.unpack_from("<3f", r, 12)
            ptype, fl, lc = struct.unpack_from("<3H", r, 24)
            solid, flags = struct.unpack_from("<2B", r, 30)
            skin, = struct.unpack_from("<i", r, 32)
            fmin, fmax = struct.unpack_from("<2f", r, 36)
            lorg = struct.unpack_from("<3f", r, 44)
            fscale, = struct.unpack_from("<f", r, 56)
            mincpu, maxcpu, mingpu, maxgpu = struct.unpack_from("<4B", r, 60)
            col = struct.unpack_from("<4B", r, 64)
            nox360, = struct.unpack_from("<?", r, 68)
            out.open("entity")
            out.kv("id", self.nid())
            out.kv("classname", "prop_static")
            out.kv("model", names[ptype])
            out.kv("origin", fmt_vec(org))
            out.kv("angles", fmt_vec(ang))
            out.kv("skin", skin)
            out.kv("solid", solid)
            out.kv("fademindist", fmt_num(fmin))
            out.kv("fademaxdist", fmt_num(fmax))
            out.kv("fadescale", fmt_num(fscale))
            out.kv("disableshadows", 1 if flags & 0x10 else 0)
            out.kv("ignorenormals", 1 if flags & 0x08 else 0)
            out.kv("disablevertexlighting", 1 if flags & 0x40 else 0)
            out.kv("disableselfshadowing", 1 if flags & 0x80 else 0)
            out.kv("screenspacefade", 1 if flags & 0x20 else 0)
            out.kv("mincpulevel", mincpu)
            out.kv("maxcpulevel", maxcpu)
            out.kv("mingpulevel", mingpu)
            out.kv("maxgpulevel", maxgpu)
            out.kv("rendercolor", "%d %d %d" % col[:3])
            out.kv("renderamt", col[3])
            out.kv("disableX360", 1 if nox360 else 0)
            if flags & 0x2:
                name = "pc_lightingorigin_%d" % k
                out.kv("lightingorigin", name)
                self.lighting_origins.append((name, lorg))
            out.close()
            self.report["static_props"] += 1
        for name, lorg in self.lighting_origins:
            out.open("entity")
            out.kv("id", self.nid())
            out.kv("classname", "info_lighting")
            out.kv("targetname", name)
            out.kv("origin", fmt_vec(lorg))
            out.close()

    def emit_cubemaps(self, out):
        for (org, size) in self.b.cubemaps:
            out.open("entity")
            out.kv("id", self.nid())
            out.kv("classname", "env_cubemap")
            out.kv("origin", fmt_vec(org))
            out.kv("cubemapsize", size)
            sides = sorted(self.sid_of_si[si] for si in self.cubemap_si.get(tuple(org), ())
                           if si in self.sid_of_si)
            out.kv("sides", " ".join(str(s) for s in sides))
            out.close()
            self.report["cubemaps"] += 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("bsp")
    ap.add_argument("vmf")
    ap.add_argument("--map-name", default=None)
    ap.add_argument("--report", default=None)
    a = ap.parse_args()
    b = Bsp(a.bsp)
    dc = Decompiler(b, a.map_name)
    text = dc.run_all()
    open(a.vmf, "w", encoding="latin-1").write(text)
    rep = dict(dc.report)
    rep["notes"] = dc.notes[:50]
    print(json.dumps(rep, indent=1))
    if a.report:
        json.dump(rep, open(a.report, "w"), indent=1)


def _run_all(self):
    text = self.run()
    self._mapping_done = True
    text = re.sub(r"@@SIDES:([0-9 ]*)@@", lambda m: self.remap_sides(m.group(1), faces=True), text)
    text = re.sub(r"@@HSIDES:([0-9 ]*)@@", lambda m: self.remap_sides(m.group(1)), text)
    return text


Decompiler.run_all = _run_all

if __name__ == "__main__":
    main()
