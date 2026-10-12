#!/usr/bin/env python3
"""pc_testbed: a compact test chamber with one bay per mechanic (bridge #29).

usage: gen_testbed.py <out.vmf> <layout.json>

This script is the single source of truth for the map: it writes the VMF and the layout JSON that
bridge/tools/gen_tb_smoke.py reads (bay bounds, standing spots, every tb_ entity, each relay and its
expected result). Test cfgs name entities, never coordinates.

Geometry: a hall (interior +-1120 u square, 384 high) with bays on its four sides. Walls are 64 u
thick (two 32 u layers so pockets can be dug 32 deep for the laser props). Slots are 320 u apart; a
one-slot bay is 256 u wide inside. The Minecraft region box is the one pc_pit_flings has (add_region).

Valve content is referenced by path only (materials, models, instances/p2editor/*.vmf in the game's
sdk_content, reached through the maps/build/game/instances symlink). Nothing of Valve's is copied.
"""
import json
import math
import sys

import add_region
from add_region import Ids

LIGHT_RGB = "183 204 218"   # pit_flings' chamber lights
BAY_B, BAY_NEAR, BAY_FAR = 22, 220, 450
BRIGHT_B = 17
HALL_B, HALL_NEAR, HALL_FAR = 12, 350, 650
HALL = 1120          # hall interior half extent
WT = 64              # wall thickness
WL = 32              # layer thickness
HALL_H = 384
PITCH = 320
SLOT_W = 256
OUT = HALL + WT      # outer plane of the hall wall: v = 0 of every bay

WHITE_W, WHITE_F, WHITE_C = "TILE/WHITE_WALL_TILE003A", "TILE/WHITE_FLOOR_TILE002A", "TILE/WHITE_CEILING_TILE002A"
BLACK_W, BLACK_F, BLACK_C = "METAL/BLACK_WALL_METAL_002C", "METAL/BLACK_FLOOR_METAL_001C", "METAL/BLACK_CEILING_METAL_001A"
NODRAW = "TOOLS/TOOLSNODRAW"
GOO = "NATURE/TOXICSLIME002A_NOREFLECT"

# side -> (into-bay unit vector, yaw of into). u axis = into rotated +90 deg.
SIDES = {"S": ((0, -1), 270), "N": ((0, 1), 90), "W": ((-1, 0), 180), "E": ((1, 0), 0)}


def rot90(v):
    return (-v[1], v[0])


# ------------------------------------------------------------------------------------------------ VMF text
class Vmf:
    def __init__(self):
        self.ids = Ids(0)
        self.solids = []   # world solids (lines)
        self.ents = []     # entity text blocks
        self.seen = set()

    def solid_lines(self, mn, mx, mats):
        """A box brush; mats is a material name or dict face->material."""
        (x0, y0, z0), (x1, y1, z1) = mn, mx
        assert x1 > x0 and y1 > y0 and z1 > z0, (mn, mx)
        faces = [
            ("top", (x0, y1, z1), (x1, y1, z1), (x1, y0, z1), "[1 0 0 0] 0.25", "[0 -1 0 0] 0.25"),
            ("bottom", (x0, y0, z0), (x1, y0, z0), (x1, y1, z0), "[1 0 0 0] 0.25", "[0 -1 0 0] 0.25"),
            ("-x", (x0, y1, z1), (x0, y0, z1), (x0, y0, z0), "[0 1 0 0] 0.25", "[0 0 -1 0] 0.25"),
            ("+x", (x1, y1, z0), (x1, y0, z0), (x1, y0, z1), "[0 1 0 0] 0.25", "[0 0 -1 0] 0.25"),
            ("+y", (x1, y1, z1), (x0, y1, z1), (x0, y1, z0), "[1 0 0 0] 0.25", "[0 0 -1 0] 0.25"),
            ("-y", (x1, y0, z0), (x0, y0, z0), (x0, y0, z1), "[1 0 0 0] 0.25", "[0 0 -1 0] 0.25"),
        ]
        out = ["\tsolid", "\t{", '\t\t"id" "%d"' % self.ids()]
        for name, p0, p1, p2, ua, va in faces:
            m = mats if isinstance(mats, str) else mats.get(name, mats.get("*", NODRAW))
            out += ["\t\tside", "\t\t{", '\t\t\t"id" "%d"' % self.ids(),
                    '\t\t\t"plane" "(%g %g %g) (%g %g %g) (%g %g %g)"' % (p0 + p1 + p2),
                    '\t\t\t"material" "%s"' % m, '\t\t\t"uaxis" "%s"' % ua, '\t\t\t"vaxis" "%s"' % va,
                    '\t\t\t"rotation" "0"', '\t\t\t"lightmapscale" "16"', '\t\t\t"smoothing_groups" "0"', "\t\t}"]
        out.append("\t}")
        return out

    def box(self, mn, mx, mats):
        key = (tuple(mn), tuple(mx), repr(mats))
        if key in self.seen:
            return
        self.seen.add(key)
        self.solids += self.solid_lines(mn, mx, mats)

    def ent(self, cls, kv=None, conns=None, solids=None):
        """kv: dict or list of pairs. conns: [(output, target, input, param, delay)]. solids: [(mn, mx, mats)]."""
        out = ["entity", "{", '\t"id" "%d"' % self.ids(), '\t"classname" "%s"' % cls]
        items = kv.items() if isinstance(kv, dict) else (kv or [])
        for k, v in items:
            out.append('\t"%s" "%s"' % (k, v))
        if conns:
            out += ["\tconnections", "\t{"]
            for c in conns:
                o, t, i = c[0], c[1], c[2]
                p = c[3] if len(c) > 3 else ""
                d = c[4] if len(c) > 4 else 0
                out.append('\t\t"%s" "%s,%s,%s,%g,-1"' % (o, t, i, p, d))
            out.append("\t}")
        for mn, mx, mats in solids or []:
            out += self.solid_lines(mn, mx, mats)
        out.append("}")
        self.ents += out


def fmt(v):
    return " ".join("%g" % x for x in v)


# ------------------------------------------------------------------------------------------------ slabs
def subtract(base, holes):
    """base rect (a0,a1,b0,b1) minus holes -> list of rects (row-merged grid decomposition)."""
    a0, a1, b0, b1 = base
    clip = []
    for h in holes:
        c = (max(h[0], a0), min(h[1], a1), max(h[2], b0), min(h[3], b1))
        if c[0] < c[1] and c[2] < c[3]:
            clip.append(c)
    As = sorted({a0, a1} | {c[0] for c in clip} | {c[1] for c in clip})
    Bs = sorted({b0, b1} | {c[2] for c in clip} | {c[3] for c in clip})
    out = []
    for j in range(len(Bs) - 1):
        run = None
        for i in range(len(As) - 1):
            ca, cb = (As[i] + As[i + 1]) / 2, (Bs[j] + Bs[j + 1]) / 2
            inside = any(c[0] < ca < c[1] and c[2] < cb < c[3] for c in clip)
            if not inside:
                if run is None:
                    run = [As[i], As[i + 1]]
                else:
                    run[1] = As[i + 1]
            elif run is not None:
                out.append((run[0], run[1], Bs[j], Bs[j + 1]))
                run = None
        if run is not None:
            out.append((run[0], run[1], Bs[j], Bs[j + 1]))
    return out


def axes_other(n):
    return [a for a in range(3) if a != n]


def slab(vmf, n, lo, hi, arng, brng, interior_hi, mat, holes=(), pockets=(), panels=()):
    """A 64 thick slab perpendicular to axis n: two 32 layers. The interior face is on the hi side if
    interior_hi. holes go through both layers, pockets only into the inner layer, panels are inlaid in
    the inner layer with their own material (rect, material)."""
    assert hi - lo == WT
    mid = lo + WL
    inner = (mid, hi) if interior_hi else (lo, mid)
    outer = (lo, mid) if interior_hi else (mid, hi)
    base = (arng[0], arng[1], brng[0], brng[1])
    a, b = axes_other(n)

    def emit(rect, rng, mats):
        mn, mx = [0, 0, 0], [0, 0, 0]
        mn[n], mx[n] = rng
        mn[a], mx[a] = rect[0], rect[1]
        mn[b], mx[b] = rect[2], rect[3]
        vmf.box(mn, mx, mats)

    face_in = {(2, True): "top", (2, False): "bottom", (1, True): "+y", (1, False): "-y", (0, True): "+x", (0, False): "-x"}[(n, interior_hi)]
    for r in subtract(base, list(holes)):
        emit(r, outer, mat)
    for r in subtract(base, list(holes) + list(pockets) + [p[0] for p in panels]):
        emit(r, inner, {"*": mat, face_in: mat})
    for r, pm in panels:
        for rr in subtract(r, list(holes)):
            emit(rr, inner, {"*": mat, face_in: pm})


# ------------------------------------------------------------------------------------------------ layout record
class Layout:
    def __init__(self):
        self.bays = {}
        self.order = []

    def bay(self, name, **kw):
        self.bays[name] = dict(name=name, entities={}, relays={}, panels={}, notes=[], **kw)
        self.order.append(name)
        return self.bays[name]


LAY = Layout()
LAY_START = {"note": "info_player_start is inside the arrival car at the bottom of its shaft; the car rises and opens (Valve's arrival logic)"}


# ------------------------------------------------------------------------------------------------ bays
class Bay:
    def __init__(self, vmf, name, side, slot, nslots, depth, height, wall=None, floor=None, ceil=None, opening=None, special=False):
        self.vmf, self.name, self.side = vmf, name, side
        self.into, self.yaw = SIDES[side]
        self.udir = rot90(self.into)
        self.yaw_u = (self.yaw + 90) % 360
        c = -960 + 320 * (slot + (nslots - 1) / 2.0)
        self.w = SLOT_W + PITCH * (nslots - 1)
        self.D, self.H = depth, height
        self.mats = (wall or WHITE_W, floor or WHITE_F, ceil or WHITE_C)
        # opening plane centre
        if side == "S": self.O = (c, -OUT)
        elif side == "N": self.O = (c, OUT)
        elif side == "W": self.O = (-OUT, c)
        else: self.O = (OUT, c)
        self.opening = opening if opening else (self.w, min(height, 224))   # (width, height)
        self.holes = {"floor": [], "ceil": [], "back": [], "left": [], "right": []}
        self.pockets = {k: [] for k in self.holes}
        self.panels = {k: [] for k in self.holes}
        self.rec = LAY.bay(name, side=side, slot=slot, nslots=nslots, depth=depth, height=height)
        self.go, self.reset = [], []   # relay outputs (target, input, param, delay)
        self.go_expect, self.reset_expect = "", ""
        self.lights = []

    # local (u: across, v: depth into the bay, z) -> world
    def w3(self, u, v, z):
        return (self.O[0] + u * self.udir[0] + v * self.into[0], self.O[1] + u * self.udir[1] + v * self.into[1], z)

    def wbox(self, u0, u1, v0, v1, z0, z1):
        pts = [self.w3(u, v, z) for u in (u0, u1) for v in (v0, v1) for z in (z0, z1)]
        return tuple(min(p[i] for p in pts) for i in range(3)), tuple(max(p[i] for p in pts) for i in range(3))

    def ang(self, local_yaw=0, pitch=0, roll=0):
        return (pitch, (self.yaw + local_yaw) % 360, roll)

    # brush helpers
    def box(self, u0, u1, v0, v1, z0, z1, mats):
        mn, mx = self.wbox(u0, u1, v0, v1, z0, z1)
        self.vmf.box(mn, mx, mats)

    def brush(self, u0, u1, v0, v1, z0, z1, mats):
        mn, mx = self.wbox(u0, u1, v0, v1, z0, z1)
        return (mn, mx, mats)

    def rect(self, kind, r):
        """local rect on a bay surface -> world (a0,a1,b0,b1) on the slab plane."""
        if kind in ("floor", "ceil"):
            z = 0 if kind == "floor" else self.H
            mn, mx = self.wbox(r[0], r[1], r[2], r[3], z, z)
            n = 2
        elif kind == "back":
            mn, mx = self.wbox(r[0], r[1], self.D, self.D, r[2], r[3])
            n = 1 if self.into[1] else 0
        else:
            u = -self.w / 2 if kind == "left" else self.w / 2
            mn, mx = self.wbox(u, u, r[0], r[1], r[2], r[3])
            n = 0 if self.into[1] else 1   # u runs along the other axis
        a, b = axes_other(n)
        return (mn[a], mx[a], mn[b], mx[b])

    def hole(self, kind, r): self.holes[kind].append(self.rect(kind, r))
    def pit(self, v0, v1, depth, u0=None, u1=None):
        """pit in the floor, v0..v1 (and u0..u1, default the full bay width), `depth` deep, walled all round."""
        hw = self.w / 2
        u0 = -hw if u0 is None else u0
        u1 = hw if u1 is None else u1
        self.hole("floor", (u0, u1, v0, v1))
        self.box(u0 - WT, u1 + WT, v0 - WT, v1 + WT, -depth - WT, -depth, BLACK_F)
        self.box(u0 - WT, u1 + WT, v0 - WT, v0, -depth, 0, BLACK_W)
        self.box(u0 - WT, u1 + WT, v1, v1 + WT, -depth, 0, BLACK_W)
        self.box(u0 - WT, u0, v0, v1, -depth, 0, BLACK_W)
        self.box(u1, u1 + WT, v0, v1, -depth, 0, BLACK_W)

    def pocket(self, kind, r): self.pockets[kind].append(self.rect(kind, r))
    def panel(self, kind, r, mat): self.panels[kind].append((self.rect(kind, r), mat))

    def record(self, name, cls, mn=None, mx=None, origin=None, **extra):
        e = {"classname": cls}
        if origin is not None: e["origin"] = [round(x, 2) for x in origin]
        if mn is not None:
            e["mins"], e["maxs"] = [round(x, 2) for x in mn], [round(x, 2) for x in mx]
            e["size"] = [round(b - a, 2) for a, b in zip(mn, mx)]
            e["top"] = round(mx[2], 2)
            e["center"] = [round((a + b) / 2, 2) for a, b in zip(mn, mx)]
        e.update(extra)
        self.rec["entities"][name] = e
        return e

    def ent(self, name, cls, u, v, z, local_yaw=0, pitch=0, roll=0, rec=True, **kv):
        o = self.w3(u, v, z)
        d = {"targetname": name, "origin": fmt(o)}
        d.update(kv)
        if "angles" not in d:
            d["angles"] = fmt(self.ang(local_yaw, pitch, roll))
        self.vmf.ent(cls, d, kv.pop("_conns", None))
        if rec and name:
            self.record(name, cls, origin=o, angles=list(self.ang(local_yaw, pitch, roll)))
        return o

    def panel_rec(self, name, kind, r, portalable=True, note=""):
        mn, mx = self.wbox(*({"floor": (r[0], r[1], r[2], r[3], 0, 0), "ceil": (r[0], r[1], r[2], r[3], self.H, self.H),
                              "back": (r[0], r[1], self.D, self.D, r[2], r[3]),
                              "left": (-self.w / 2, -self.w / 2, r[0], r[1], r[2], r[3]),
                              "right": (self.w / 2, self.w / 2, r[0], r[1], r[2], r[3])}[kind]))
        normal = {"floor": (0, 0, 1), "ceil": (0, 0, -1), "back": (-self.into[0], -self.into[1], 0),
                  "left": (self.udir[0], self.udir[1], 0), "right": (-self.udir[0], -self.udir[1], 0)}[kind]
        e = self.record(name, "info_target", mn, mx, origin=[(a + b) / 2 for a, b in zip(mn, mx)], panel=kind, portalable=portalable, normal=list(normal), note=note)
        self.vmf.ent("info_target", {"targetname": name, "origin": fmt([(a + b) / 2 for a, b in zip(mn, mx)])})
        return e

    def light(self, u, v, z, level=None, near=None, far=None, name=None, start_off=False):
        level = BAY_B if level is None else level
        near = BAY_NEAR if near is None else near
        far = BAY_FAR if far is None else far
        o = self.w3(u, v, z)
        kv = {"origin": fmt(o), "_light": "%s %d" % (LIGHT_RGB, level), "_lightHDR": "-1 -1 -1 1", "_lightscaleHDR": "1",
              "_constant_attn": "0", "_linear_attn": "0", "_quadratic_attn": "1", "_fifty_percent_distance": str(near),
              "_zero_percent_distance": str(far), "_hardfalloff": "0", "style": "0", "spawnflags": "1" if start_off else "0"}
        if name:
            kv["targetname"] = name
            self.record(name, "light", origin=o)
        self.vmf.ent("light", kv)

    def lamp_pair(self, nm, lamp_u=0, lamp_v=None, z=None):
        pass

    def relays(self):
        """tb_go_<bay> and tb_reset_<bay>"""
        for kind, lst, expect in (("go", self.go, self.go_expect), ("reset", self.reset, self.reset_expect)):
            nm = "tb_%s_%s" % (kind, self.name)
            conns = [("OnTrigger", t, i, p, d) for (t, i, p, d) in lst]
            self.vmf.ent("logic_relay", {"targetname": nm, "origin": fmt(self.w3(-self.w / 2 + 32, 32 if kind == "go" else 64, 8)), "spawnflags": "2"}, conns)
            self.rec["relays"][kind] = {"name": nm, "fires": ["%s %s %s" % (t, i, p) for (t, i, p, d) in lst], "expect": expect}

    # ----------------------------------------------------------------- shell
    def emit(self):
        v = self.vmf
        wm, fm, cm = self.mats
        hw = self.w / 2
        U0, U1 = -hw - WT, hw + WT
        # floor
        def wr(u0, u1, v0, v1, z0, z1):
            return self.wbox(u0, u1, v0, v1, z0, z1)
        mn, mx = wr(U0, U1, 0, self.D + WT, -WT, 0)
        slab(v, 2, mn[2], mx[2], (mn[0], mx[0]), (mn[1], mx[1]), True, fm, self.holes["floor"], self.pockets["floor"], self.panels["floor"])
        mn, mx = wr(U0, U1, 0, self.D + WT, self.H, self.H + WT)
        slab(v, 2, mn[2], mx[2], (mn[0], mx[0]), (mn[1], mx[1]), False, cm, self.holes["ceil"], self.pockets["ceil"], self.panels["ceil"])
        # back wall (interior toward the hall)
        mn, mx = wr(U0, U1, self.D, self.D + WT, -WT, self.H + WT)
        n = 1 if self.into[1] else 0
        a, b = axes_other(n)
        slab(v, n, mn[n], mx[n], (mn[a], mx[a]), (mn[b], mx[b]), (self.into[n] < 0), wm, self.holes["back"], self.pockets["back"], self.panels["back"])
        # side walls
        for kind, (u0, u1) in (("left", (-hw - WT, -hw)), ("right", (hw, hw + WT))):
            mn, mx = wr(u0, u1, 0, self.D + WT, -WT, self.H + WT)
            n = 0 if self.into[1] else 1
            a, b = axes_other(n)
            interior_hi = (kind == "left") == ((self.udir[n] > 0))   # left wall's interior is at +u
            slab(v, n, mn[n], mx[n], (mn[a], mx[a]), (mn[b], mx[b]), interior_hi, wm, self.holes[kind], self.pockets[kind], self.panels[kind])
        if self.H + WT > HALL_H + WT:   # taller than the hall: the front above the hall wall needs closing
            self.box(-hw - WT, hw + WT, -WT, 0, HALL_H, self.H + WT, wm)

    # opening in the hall wall: world rect on the hall slab
    def opening_rect(self):
        ow, oh = self.opening
        return self.rect_hall(ow, oh)

    def rect_hall(self, ow, oh):
        mn, mx = self.wbox(-ow / 2, ow / 2, -WT, 0, 0, oh)
        axis = 1 if self.into[1] else 0
        a = 0 if axis == 1 else 1
        return (mn[a], mx[a], mn[2], mx[2])

    def record_bay(self):
        mn, mx = self.wbox(-self.w / 2, self.w / 2, 0, self.D, 0, self.H)
        self.rec["interior"] = {"min": list(mn), "max": list(mx)}
        omn, omx = self.wbox(-self.w / 2 - WT, self.w / 2 + WT, -WT, self.D + WT, -WT, self.H + WT)
        self.rec["bounds"] = {"min": list(omn), "max": list(omx)}
        spot = self.w3(0, getattr(self, "spot_v", -160), 8)   # (bridge #32: the bridge bay's spot is inside the bay, see there)
        self.rec["spot"] = {"name": "tb_spot_" + self.name, "origin": list(spot), "yaw": self.yaw}
        self.rec["inside"] = {"origin": list(self.w3(0, 96, 8)), "yaw": self.yaw}
        self.vmf.ent("info_target", {"targetname": "tb_spot_" + self.name, "origin": fmt(spot), "angles": fmt((0, self.yaw, 0))})
        self.vmf.ent("env_cubemap", {"origin": fmt(self.w3(0, self.D / 2, min(self.H / 2, 128))), "cubemapsize": "0"})
        self.rec["u_dir"] = list(self.udir)
        self.rec["into_dir"] = list(self.into)


def ln(vmf):
    return None


MAIN = {"portals": "tb_panel_wall_back", "fling": "tb_panel_floor_fling", "arrival": "arrival_elevator-elevator_1", "mc": "tb_panel_wall_mc", "dark": "tb_dark_wall",
        "bright": "tb_bright_wall", "lift": "tb_lift", "doorh": "tb_door_h", "mlinear": "tb_movelinear", "departure": "@exit_door", "train": "tb_train",
        "rotate": "tb_turntable", "tilt": "tb_tilt", "doorpanel": "tb_doorpanel_lift", "arm": "tb_arm-model_arms", "cubes": "tb_btn_floor", "laser": "tb_laser_emitter",
        "bridge": "tb_bridge", "funnel": "tb_funnel", "faith": "tb_faith_arm", "fizzler": "tb_fizzler_model", "turret": "tb_turret", "goo": "tb_goo_surface",
        "doors": "tb_door", "gel": "tb_gel"}
SETTLE = {"bridge": 4, "funnel": 3, "lift": 5, "doorh": 3, "mlinear": 5, "train": 14, "rotate": 6, "tilt": 4, "doorpanel": 5, "arm": 5, "arrival": 4, "departure": 3, "cubes": 4, "turret": 4, "doors": 3, "gel": 4}
RIDE = {"lift", "mlinear", "train", "rotate", "tilt", "doorpanel", "arrival"}
RIDE_AT = {"rotate": (64, 256, 16)}   # (bridge #31: pcb_stand_on @x,y,z stands exactly there: 64 u off the turntable's pivot, so the rider goes round)


# ================================================================================================ build
def build(vmf):
    bays = []

    def B(*a, **k):
        b = Bay(vmf, *a, **k)
        bays.append(b)
        return b

    # ------------------------------------------------------------------------------------------ S side
    # portals: wall-to-wall pair, panels, non-portalable neighbours
    b = B("portals", "S", 0, 2, 512, 320, wall=BLACK_W, floor=BLACK_F, ceil=BLACK_C)
    hw = b.w / 2   # 288
    b.panel("left", (192, 320, 0, 128), WHITE_W); b.panel_rec("tb_panel_wall_a", "left", (192, 320, 0, 128))   # (bridge #31: from the floor up, so the pair is walkable)
    b.panel("right", (192, 320, 0, 128), WHITE_W); b.panel_rec("tb_panel_wall_b", "right", (192, 320, 0, 128))
    b.panel("back", (-64, 64, 32, 160), WHITE_W); b.panel_rec("tb_panel_wall_back", "back", (-64, 64, 32, 160))
    b.panel_rec("tb_nop_wall_back", "back", (64, 192, 32, 160), portalable=False, note="black metal next to the white back panel")
    b.panel("floor", (-64, 64, 128, 256), WHITE_F); b.panel_rec("tb_panel_floor", "floor", (-64, 64, 128, 256))
    b.panel_rec("tb_nop_floor", "floor", (-64, 64, 256, 384), portalable=False, note="black metal floor behind the white floor panel")
    b.panel("ceil", (-64, 64, 128, 256), WHITE_C); b.panel_rec("tb_panel_ceiling", "ceil", (-64, 64, 128, 256))
    # (bridge #32) LinkageGroupID: Portal 2 pairs one portal per colour per group, and group 0 is the player's gun pair, so the map's own pairs
    # have groups of their own: tb_pair_a/b = 1, tb_fling_floor_portal / tb_fling_wall_portal = 2. Group 0 is left to the gun.
    pa = b.w3(-hw + 1, 256, 64); pb = b.w3(hw - 1, 256, 64)
    vmf.ent("prop_portal", {"targetname": "tb_pair_a", "origin": fmt(pa), "angles": fmt(b.ang(90)), "Activated": "0", "PortalTwo": "0", "LinkageGroupID": "1"})
    vmf.ent("prop_portal", {"targetname": "tb_pair_b", "origin": fmt(pb), "angles": fmt(b.ang(270)), "Activated": "0", "PortalTwo": "1", "LinkageGroupID": "1"})
    b.rec["entities"]["tb_pair_a"] = dict(classname="prop_portal", linkage=1, origin=list(pa), normal=[b.udir[0], b.udir[1], 0], note="left wall (u=-288) at v=256, z=64 (bottom on the floor: walk into it), faces +u")
    b.rec["entities"]["tb_pair_b"] = dict(classname="prop_portal", linkage=1, origin=list(pb), normal=[-b.udir[0], -b.udir[1], 0], note="right wall (u=+288), faces -u")
    b.go = [("tb_pair_a", "SetActivatedState", "1", 0), ("tb_pair_b", "SetActivatedState", "1", 0.1)]
    b.reset = [("tb_pair_a", "SetActivatedState", "0", 0), ("tb_pair_b", "SetActivatedState", "0", 0)]
    b.go_expect = "both wall portals open: walking into tb_pair_a comes out of tb_pair_b (and back), 2*hw = 576 u apart"
    b.reset_expect = "both portals closed"
    for u, vv in ((-144, 128), (144, 128), (-144, 384), (144, 384)):
        b.light(u, vv, 296)

    # fling: high ledge, floor panel, wall panel
    b = B("fling", "S", 2, 1, 640, 832)
    b.box(-128, 128, 480, 640, 0, 640, {"*": WHITE_W, "top": WHITE_F})
    b.rec["entities"]["tb_fling_ledge"] = dict(classname="brush", top=640, mins=list(b.wbox(-128, 128, 480, 640, 0, 640)[0]), maxs=list(b.wbox(-128, 128, 480, 640, 0, 640)[1]),
                                              note="standing surface z=640, 160 deep; tb_spot_fling_high is on it")
    b.panel("floor", (-64, 64, 128, 256), WHITE_F); b.panel_rec("tb_panel_floor_fling", "floor", (-64, 64, 128, 256))
    b.panel("left", (256, 384, 0, 128), WHITE_W); b.panel_rec("tb_panel_wall_fling", "left", (256, 384, 0, 128))
    # (bridge #34) the pair sits ON its surfaces, as Valve places its floor/ceiling prop_portals: sp_a1_intro6's room_2_fling_portal (floor, angles "-90 0 0") has
    # its origin 0.25 u from the brush face (z -632), sp_a1_intro4's ceiling portal 0.0 (read from the BSPs' entity lumps and trees; wall portals there range 0..8 u). This floor
    # portal was 1 u above the floor: Portal 2 moved it to z 0 when it activated (tbtrace full3: z 1.0 -> 0.0) and Chell then landed on it and stood
    # there (no transit in full1, full2 or full3), while the wall pair, which Portal 2 leaves alone, works for her.
    fl = b.w3(0, 192, 0)
    wp = b.w3(-128, 320, 64)
    # (bridge #37) tbtest full4: still no transit for Chell (she lands on the open floor portal at z 0.0 with v 0), with the pair ON its surfaces like Valve's floor
    # portal (sp_a1_intro6 room_2_fling_portal: z -632 = the face of the brush under it, measured from the BSP). What differs from the wall pair that works for
    # her (tb_pair_a/b, group 1): the floor portal is BLUE and is activated FIRST; Valve's static floor portals are all orange (PortalTwo 1) and wait for a
    # partner that exists already (intro6: the player's blue portal). So: the floor portal is orange (PortalTwo 1), the wall portal blue (PortalTwo 0), the wall
    # is activated first and the floor second, and both are asserted again 0.6 s later (a re-run of Portal 2's linkage, the same thing clip.cpp's Redirect
    # does for a floor portal whose partner was placed after it). The plugin also watches every pair: an active portal whose m_hLinkedPortal is empty while
    # its group has an active partner gets SetActivatedState again and the engine's view is logged (mcmechanics.cpp PortalLinkWatch, "linkwatch").
    vmf.ent("prop_portal", {"targetname": "tb_fling_floor_portal", "origin": fmt(fl), "angles": "-90 0 0", "Activated": "0", "PortalTwo": "1", "LinkageGroupID": "2"})
    vmf.ent("prop_portal", {"targetname": "tb_fling_wall_portal", "origin": fmt(wp), "angles": fmt(b.ang(90)), "Activated": "0", "PortalTwo": "0", "LinkageGroupID": "2"})
    b.rec["entities"]["tb_fling_floor_portal"] = dict(classname="prop_portal", linkage=2, origin=list(fl), normal=[0, 0, 1])
    b.rec["entities"]["tb_fling_wall_portal"] = dict(classname="prop_portal", linkage=2, origin=list(wp), normal=list(b.udir) + [0])
    b.rec["entities"]["tb_spot_fling_high"] = dict(classname="info_target", origin=list(b.w3(0, 560, 640 + 8)), note="top of the ledge, 640 u above the floor panel")
    vmf.ent("info_target", {"targetname": "tb_spot_fling_high", "origin": fmt(b.w3(0, 560, 648)), "angles": fmt((0, b.yaw + 180, 0))})
    # (bridge #39) tbtest fix6: the SAME floor and wall spots work when the player's gun pair (group 0) is put there with `portal_place` (fling.fling_gun.chell
    # PASSES: transit 1 at z -30.8, 870 u/s in and out, the portals at z 0.5 and 1 u off the wall), and fail when this pair is activated by the map (the portals
    # report linked, Chell lands on the open floor portal at z 0.0 with v 0). Differences between the two runs: the group (0 / 2: ruled out by tb_pair_a/b,
    # group 1, which Chell walks through), the activation path (an input on a static portal / the placement command), and the offset: the gun pair sat 0.5 u
    # above the floor and 1 u in front of the wall, this one exactly on the surface. So after the activation the map PLACES both portals the way the placement
    # command does, with the input Portal 2 gives every portal for it (NewLocation "x y z pitch yaw roll", the one clip.cpp's Redirect already uses on a group 0
    # portal), at the gun pair's proven offsets (floor +0.5 u, wall +1 u along the normal), then asserts the activation once more.
    PLACE_FLOOR = "%.2f %.2f %.2f -90 0 0" % (fl[0], fl[1], fl[2] + 0.5)
    wn = (b.udir[0], b.udir[1])
    wy = b.ang(90)[1]
    PLACE_WALL = "%.2f %.2f %.2f 0 %.1f 0" % (wp[0] + wn[0] * 1.0, wp[1] + wn[1] * 1.0, wp[2], wy)
    b.go = [("tb_fling_wall_portal", "SetActivatedState", "1", 0), ("tb_fling_floor_portal", "SetActivatedState", "1", 0.1),
            ("tb_fling_wall_portal", "NewLocation", PLACE_WALL, 0.3), ("tb_fling_floor_portal", "NewLocation", PLACE_FLOOR, 0.4),
            ("tb_fling_wall_portal", "SetActivatedState", "1", 0.6), ("tb_fling_floor_portal", "SetActivatedState", "1", 0.7)]
    b.reset = [("tb_fling_floor_portal", "SetActivatedState", "0", 0), ("tb_fling_wall_portal", "SetActivatedState", "0", 0)]
    b.go_expect = "floor portal on tb_panel_floor_fling and wall portal on tb_panel_wall_fling open; dropping from tb_spot_fling_high (640 u up, ~877 u/s) flings out of the wall portal across the bay"
    b.reset_expect = "both closed"
    for z in (200, 500, 780):
        b.light(0, 300, z)

    # arrival elevator (special)
    arrival = ElevatorBay(vmf, "arrival", "S", 3)
    bays.append(arrival)

    # mc region
    b = B("mc", "S", 4, 1, 512, 320, wall=BLACK_W, floor=BLACK_F, ceil=BLACK_C)
    b.panel("back", (-96, 96, 0, 192), WHITE_W); b.panel_rec("tb_panel_wall_mc", "back", (-96, 96, 0, 192), note="big wall for the G-portal / region portal")
    b.panel("floor", (-96, 96, 192, 384), WHITE_F); b.panel_rec("tb_panel_floor_mc", "floor", (-96, 96, 192, 384), note="floor portal into Minecraft")
    b.panel("left", (96, 224, 0, 128), WHITE_W); b.panel_rec("tb_panel_wall_mc2", "left", (96, 224, 0, 128))
    b.light(0, 128, 296, name="tb_mc_lamp", start_off=True)
    b.light(0, 384, 296)
    b.go = [("tb_mc_lamp", "TurnOn", "", 0)]; b.reset = [("tb_mc_lamp", "TurnOff", "", 0)]
    b.go_expect = "extra lamp tb_mc_lamp lights the bay (nothing region related moves: portals are player placed)"
    b.reset_expect = "lamp off"

    # dark
    b = B("dark", "S", 5, 1, 512, 256, wall=BLACK_W, floor=BLACK_F, ceil=BLACK_C, opening=(192, 160))
    b.light(0, 400, 240, name="tb_dark_lamp", start_off=True)
    b.panel_rec("tb_dark_wall", "back", (-96, 96, 32, 160), portalable=False, note="dark bay back wall; no lights at all until tb_go_dark")
    b.go = [("tb_dark_lamp", "TurnOn", "", 0)]; b.reset = [("tb_dark_lamp", "TurnOff", "", 0)]
    b.go_expect = "a weak lamp at the far end (v=400 z=240) comes on; before, the bay is lit only by hall light spilling in"
    b.reset_expect = "lamp off (dark again)"

    # bright
    b = B("bright", "S", 6, 1, 512, 256)
    for vv in (100, 250, 400):
        b.light(0, vv, 240, level=BRIGHT_B, near=260, far=520)
    b.light(0, 250, 40, name="tb_bright_lamp", start_off=True)
    b.panel_rec("tb_bright_wall", "back", (-96, 96, 32, 160), note="bright bay: white walls, three strong ceiling lights")
    b.go = [("tb_bright_lamp", "TurnOn", "", 0)]; b.reset = [("tb_bright_lamp", "TurnOff", "", 0)]
    b.go_expect = "a floor-level extra lamp lights the bay even brighter"
    b.reset_expect = "extra lamp off"

    # ------------------------------------------------------------------------------------------ N side
    # lift: func_door vertical
    b = B("lift", "N", 0, 1, 512, 384)
    lift = b.brush(-80, 80, 176, 336, 0, 16, {"*": BLACK_W, "top": BLACK_F, "bottom": BLACK_F})
    vmf.ent("func_door", dict(targetname="tb_lift", movedir="-90 0 0", speed="64", lip="-176", wait="-1", spawnflags="0", forceclosed="0", dmg="0", origin=fmt(b.w3(0, 256, 8))), solids=[lift])
    b.record("tb_lift", "func_door", lift[0], lift[1], movedir="up", travel=192, speed=64)
    b.go = [("tb_lift", "Open", "", 0)]; b.reset = [("tb_lift", "Close", "", 0)]
    b.go_expect = "platform top goes z 16 -> 208 (192 u) at 64 u/s (3 s); ceiling is 384"
    b.reset_expect = "platform back to z 16"
    b.light(0, 150, 372); b.light(0, 400, 372)

    # horizontal door
    b = B("doorh", "N", 1, 1, 512, 256)
    for (u0, u1) in ((-b.w / 2, -64), (64, b.w / 2)):
        b.box(u0, u1, 256, 288, 0, 256, WHITE_W)
    b.box(-64, 64, 256, 288, 128, 256, WHITE_W)
    slab_door = b.brush(-64, 64, 240, 256, 0, 128, {"*": BLACK_W})
    ym = (b.yaw_u) % 360
    vmf.ent("func_door", dict(targetname="tb_door_h", movedir="0 %d 0" % ym, speed="64", lip="0", wait="-1", spawnflags="0", dmg="0", origin=fmt(b.w3(0, 248, 64))), solids=[slab_door])
    b.record("tb_door_h", "func_door", slab_door[0], slab_door[1], movedir="+u", travel=128, speed=64)
    b.go = [("tb_door_h", "Open", "", 0)]; b.reset = [("tb_door_h", "Close", "", 0)]
    b.go_expect = "sliding slab moves +u by 128 u (2 s), opening the 128 x 128 doorway in the partition at v=256..288"
    b.reset_expect = "slab back across the doorway"
    b.light(0, 130, 240); b.light(0, 380, 240)

    # movelinear
    b = B("mlinear", "N", 2, 1, 512, 256)
    pl = b.brush(-64, 64, 64, 192, 0, 16, {"*": BLACK_W, "top": BLACK_F})
    vmf.ent("func_movelinear", dict(targetname="tb_movelinear", movedir="0 %d 0" % b.yaw, movedistance="192", speed="64", startposition="0", spawnflags="0", blockdamage="0", origin=fmt(b.w3(0, 128, 8))), solids=[pl])
    b.record("tb_movelinear", "func_movelinear", pl[0], pl[1], movedir="+v (into the bay)", travel=192, speed=64)
    b.go = [("tb_movelinear", "Open", "", 0)]; b.reset = [("tb_movelinear", "Close", "", 0)]
    b.go_expect = "128 x 128 platform slides 192 u deeper into the bay (3 s)"
    b.reset_expect = "back to v 64..192"
    b.light(0, 150, 240); b.light(0, 400, 240)

    # departure elevator
    dep = ElevatorBay(vmf, "departure", "N", 3)
    bays.append(dep)

    # tracktrain
    b = B("train", "N", 4, 1, 576, 256)
    tr = b.brush(-64, 64, 32, 160, 0, 16, {"*": BLACK_W, "top": BLACK_F})
    vmf.ent("func_tracktrain", dict(targetname="tb_train", target="tb_train_p1", speed="0", startspeed="100", wheels="50", height="0", bank="0", dmg="0", spawnflags="17", orientationtype="0", origin=fmt(b.w3(0, 96, 8))), solids=[tr])
    p = [(96, "tb_train_p1", "tb_train_p2"), (288, "tb_train_p2", "tb_train_p3"), (480, "tb_train_p3", None)]
    for vv, nm, nxt in p:
        kv = dict(targetname=nm, origin=fmt(b.w3(0, vv, 8)), spawnflags="0", orientationtype="0")
        if nxt: kv["target"] = nxt
        conns = [("OnPass", "tb_train", "Stop", "", 0), ("OnPass", "tb_train", "Resume", "", 2.0)] if nm == "tb_train_p2" else None
        vmf.ent("path_track", kv, conns)
        b.record(nm, "path_track", origin=b.w3(0, vv, 8))
    b.record("tb_train", "func_tracktrain", tr[0], tr[1], speed=100, stops="tb_train_p2 (2 s wait), tb_train_p3 (end)")
    b.go = [("tb_train", "StartForward", "", 0)]
    b.reset = [("tb_train", "Stop", "", 0), ("tb_train", "TeleportToPathNode", "tb_train_p1", 0.1)]
    b.go_expect = "train (initial speed 0, max speed 100) rides p1 (v=96) -> p2 (v=288, pauses 2 s) -> p3 (v=480, ends), 100 u/s"
    b.reset_expect = "train stopped and teleported back to p1"
    b.light(0, 150, 240); b.light(-144, 400, 240); b.light(144, 400, 240)

    # rotating
    b = B("rotate", "N", 5, 1, 512, 256)
    disc = b.brush(-96, 96, 160, 352, 0, 16, {"*": BLACK_W, "top": BLACK_F})
    vmf.ent("func_rotating", dict(targetname="tb_turntable", maxspeed="20", fanfriction="20", volume="0", spawnflags="0", dmg="0", origin=fmt(b.w3(0, 256, 8))), solids=[disc])
    b.record("tb_turntable", "func_rotating", disc[0], disc[1], maxspeed="20 deg/s about z", pivot=list(b.w3(0, 256, 8)))
    b.go = [("tb_turntable", "Start", "", 0)]; b.reset = [("tb_turntable", "Stop", "", 0)]
    b.go_expect = "192 x 192 disc spins at 20 deg/s about z (accelerates over ~2 s)"
    b.reset_expect = "disc stops (stays at whatever angle it reached)"
    b.light(0, 130, 240); b.light(0, 400, 240)

    # tilting platform
    # (bridge #37) tbtest full4: tilt.ride.chell AND tilt.ride.mc were BLOCKED, the platform stayed at 0 deg with a rider on it, while tilt.control.chell (nobody
    # on it) turned to 20 deg. A brush func_door_rotating is a Source "pusher": a player standing on it rests on its top with no clearance, and any turn
    # pushes the axis-aligned hull's uphill edge into the tilted plane, so the door is blocked on the first tick (Source behaviour, not a bridge bug).
    # Valve's moving floors with riders are not brush pushers: they are animated or parented prop_dynamic arm plates (solid 6, vphysics collision) on a
    # carrier that does not collide: doorpanel below is that for a lift (a func_door parent, arm_64x64_justtop plates). The tilting platform is the same:
    # `tb_tilt` is a PASSABLE func_door_rotating (spawnflag 8: not solid, never blocked, it only turns) with nine of Valve's flat 64x64 plates
    # (arm_64x64_justtop, solid 6) parented to it, 192 x 192 as before, hinged on the same edge. The plates' own collision turns with it.
    b = B("tilt", "N", 6, 1, 512, 256)
    axis_flag = 64 if b.into[1] else 128   # world x or y axis (u runs along it)
    hinge = b.w3(0, 128, 8)
    rootb = b.brush(-8, 8, 128, 144, 0, 16, "TOOLS/TOOLSNODRAW")   # (the root needs a brush; it is not solid)
    vmf.ent("func_door_rotating", dict(targetname="tb_tilt", distance="20", speed="10", wait="-1", spawnflags=str(axis_flag | 8), dmg="0", forceclosed="0", origin=fmt(hinge)), solids=[rootb])
    # (bridge #39) tbtest full5/fix6: the nine arm_64x64_justtop plates were no walking surface at all. That model is the framework of an arm's top (a lattice
    # with arm bodies under it, see the screenshot of tilt.control: the panel itself is a separate func_brush in Valve's chambers), so a player standing 10 u
    # off a plate's centre fell through its open middle to the floor (Chell at z 0.0 for the whole run, the plates tilting above her). The surface is a real
    # solid panel now: one 192 x 192 x 16 func_brush parented to the passable root (the same carrier, the same hinge, the same 20 degrees).
    # Honest note for the lead: the root is a Source pusher and a rider on a pusher that TURNS about a horizontal axis was blocked in tbtest full4 (the brush
    # door itself stayed at 0.0 deg with Chell standing on it; with nobody on it it turned to 20): that was the solid brush as the pusher. If this panel under
    # a passable root is blocked the same way, Valve's own way is below (tb_ramp: an arm whose animation moves a panel, no pusher), and tilt.ride is then
    # not a Source behaviour at all.
    pm = b.brush(-96, 96, 128, 320, 0, 16, {"*": BLACK_W, "top": WHITE_F})
    pc = tuple((a + c) / 2 for a, c in zip(pm[0], pm[1]))
    vmf.ent("func_brush", dict(targetname="tb_tilt_panel", parentname="tb_tilt", origin=fmt(pc), Solidity="0", solidbsp="0", spawnflags="2", StartDisabled="0", disableshadows="1"), solids=[pm])
    pmn, pmx = b.wbox(-96, 96, 128, 320, 0, 16)
    b.record("tb_tilt", "func_door_rotating", pmn, pmx, hinge=list(hinge), tilt="20 deg about the u axis at v=128, 10 deg/s",
             note="(bridge #39) passable root (it only turns) carrying tb_tilt_panel, a 192 x 192 x 16 solid func_brush (top z 16); the lattice plates are gone")
    b.record("tb_tilt_panel", "func_brush", pmn, pmx, parent="tb_tilt", note="(bridge #39) the solid walking surface, parented to tb_tilt")
    # (bridge #39) Valve's own tilting/rising panel, for comparison and for the tests to try: an arm (prop_dynamic arm64x64_interior_rusty, solid 6, animated: it
    # is NOT a pusher; its collision is the model's bone followers) and a func_brush panel parented to the arm's `panel_attach` attachment (the pattern of
    # instances/p2editor/drawbridge_196_01a.vmf and the tilex2 wall panels of sp_a2_pit_flings: "SetParentAttachmentMaintainOffset panel_attach" at map start). The
    # sequence rampa_stair01_up / _down of arm64x64_interior_animation.mdl (decoded offline: 41 frames at 24 fps = 1.7 s; the attachment turns the panel
    # 15 degrees about the arm's local y axis and carries it 22.7 u along the arm's local z and 5 u along its local x). The arm stands with local x up (pitch -90),
    # local z along +v: the panel starts flat with its top at z 16, then slides 23 u toward the back wall, rising 5 u and tipping 15 degrees.
    rd = b.into
    arm_yaw = math.degrees(math.atan2(-rd[1], -rd[0])) % 360
    ro = b.w3(0, 428, 16)
    rp = b.brush(-48, 48, 380, 476, 8, 16, {"*": BLACK_W, "top": WHITE_F})
    rpc = tuple((a + c) / 2 for a, c in zip(rp[0], rp[1]))
    vmf.ent("prop_dynamic", dict(targetname="tb_ramp_arm", model="models/anim_wp/room_transform/arm64x64_interior_rusty.mdl", origin=fmt(ro), angles=fmt((-90, arm_yaw, 0)), solid="6",
                                 DefaultAnim="rampa_stair01_down_idleend", disableshadows="1", DisableBoneFollowers="0"))
    vmf.ent("func_brush", dict(targetname="tb_ramp", parentname="tb_ramp_arm", origin=fmt(rpc), Solidity="0", solidbsp="0", spawnflags="2", StartDisabled="0", disableshadows="1"), solids=[rp])
    vmf.ent("logic_auto", {"spawnflags": "1", "origin": fmt(ro)}, [("OnMapSpawn", "tb_ramp", "SetParentAttachmentMaintainOffset", "panel_attach", 0.2)])
    rmn, rmx = b.wbox(-48, 48, 380, 476, 8, 16)
    b.record("tb_ramp_arm", "prop_dynamic", origin=ro, angles=[-90, arm_yaw, 0], note="(bridge #39) Valve's arm64x64_interior_rusty (solid 6), animated by SetAnimation rampa_stair01_up / _down")
    b.record("tb_ramp", "func_brush", rmn, rmx, parent="tb_ramp_arm", tilt="15 deg and about +23 u along +v, +5 u up over 1.7 s (rampa_stair01_up)",
             note="(bridge #39) 96 x 96 x 8 solid panel (rest top z 16) attached to the arm's panel_attach: Valve's moving-panel pattern, no Source pusher")
    b.go = [("tb_tilt", "Open", "", 0), ("tb_ramp_arm", "SetAnimation", "rampa_stair01_up", 0), ("tb_ramp_arm", "SetDefaultAnimation", "rampa_stair01_up_idleend", 0.1)]
    b.reset = [("tb_tilt", "Close", "", 0), ("tb_ramp_arm", "SetAnimation", "rampa_stair01_down", 0), ("tb_ramp_arm", "SetDefaultAnimation", "rampa_stair01_down_idleend", 0.1)]
    b.go_expect = "192 x 192 panel (tb_tilt_panel) tilts 20 degrees about its v=128 edge in 2 s; at the back of the bay the arm panel tb_ramp slides 23 u, rises 5 u and tips 15 degrees in 1.7 s"
    b.reset_expect = "both back to level"
    b.light(0, 130, 240); b.light(0, 400, 240)

    # ------------------------------------------------------------------------------------------ W side
    # door + parented prop_dynamic panel
    b = B("doorpanel", "W", 0, 1, 512, 384)
    b.pit(144, 304, 200, -80, 80)
    dp = b.brush(-64, 64, 160, 288, -128, 0, "TOOLS/TOOLSNODRAW")
    vmf.ent("func_door", dict(targetname="tb_doorpanel_lift", movedir="-90 0 0", speed="48", lip="0", wait="-1", spawnflags="0", dmg="0", origin=fmt(b.w3(0, 224, -64))), solids=[dp])
    # (bridge #31: arm_interior_128 is the inside of an arm, an upright ring; the platform is four of Valve's flat 64x64 panel tops, origin = top face)
    for k, (pu, pv) in enumerate(((-32, 192), (32, 192), (-32, 256), (32, 256))):
        vmf.ent("prop_dynamic", dict(targetname="tb_doorpanel_model" if k == 0 else "tb_doorpanel_model_%d" % (k + 1), parentname="tb_doorpanel_lift",
                                     model="models/anim_wp/room_transform/arm_64x64_justtop.mdl", origin=fmt(b.w3(pu, pv, 0)), angles=fmt(b.ang(0, 0, 0)), solid="6", disableshadows="1"))
    b.record("tb_doorpanel_lift", "func_door", dp[0], dp[1], movedir="up", travel=128, speed=48, child="tb_doorpanel_model", top0=0)
    b.record("tb_doorpanel_model", "prop_dynamic", origin=b.w3(-32, 192, 0), parent="tb_doorpanel_lift", note="arm_64x64_justtop (solid 6) x4 parented to the nodraw door cube, top flush with the floor (the first is this one)")
    b.go = [("tb_doorpanel_lift", "Open", "", 0)]; b.reset = [("tb_doorpanel_lift", "Close", "", 0)]
    b.go_expect = "nodraw func_door cube (z -128..0) rises 128 u at 48 u/s carrying four parented arm_64x64_justtop plates; the walkable top goes z 0 -> 128"
    b.reset_expect = "back down"
    b.light(0, 130, 372); b.light(0, 400, 372)

    # arm panels (instance 'stairs': arm_8panel + ramp door + func_brush steps)
    b = B("arm", "W", 1, 1, 512, 256)
    st = b.w3(-96, 160, 64)
    vmf.ent("func_instance", dict(targetname="tb_arm", file="instances/p2editor/stairs.vmf", origin=fmt(st), angles=fmt(b.ang(0)), replace01="$connectioncount 1", replace02="$start_deployed 0"))
    b.record("tb_arm", "func_instance", origin=st, note="stairs.vmf: arm_8panel ramp, 4 func_brush steps on it; inner names tb_arm-*")
    b.go = [("tb_arm-counter", "Add", "1", 0)]; b.reset = [("tb_arm-counter", "Subtract", "1", 0)]
    b.go_expect = "tb_arm-model_arms animates (SetPlaybackRate/animation): the 8-panel arm unfolds into a stair ramp along +v"
    b.reset_expect = "arm folds back down"
    b.light(0, 130, 240); b.light(0, 400, 240)

    # cube dropper + buttons
    b = B("cubes", "W", 2, 1, 512, 320)
    cube = b.w3(0, 256, 224)
    vmf.ent("prop_weighted_cube", dict(targetname="tb_cube", origin=fmt(cube), angles="0 0 0", NewSkins="1", allowfunnel="1", PaintPower="4"), [("OnFizzled", "tb_cube_template", "ForceSpawn", "", 0.5)])
    vmf.ent("point_template", dict(targetname="tb_cube_template", Template01="tb_cube", spawnflags="2", origin=fmt(cube)))
    b.record("tb_cube", "prop_weighted_cube", origin=cube, note="starts 224 u up and falls; ForceSpawn of tb_cube_template drops another")
    b.record("tb_cube_template", "point_template", origin=cube)
    fb = b.w3(-96, 160, 0)
    vmf.ent("prop_floor_button", dict(targetname="tb_btn_floor", model="models/props/portal_button.mdl", origin=fmt(fb), angles="0 0 0"),
            [("OnPressed", "tb_btn_lamp", "TurnOn", "", 0), ("OnUnPressed", "tb_btn_lamp", "TurnOff", "", 0), ("OnPressed", "tb_cnt_floor", "Add", "1", 0)])
    b.record("tb_btn_floor", "prop_floor_button", origin=fb, note="pressed by the cube or a player; lights tb_btn_lamp")
    pbp = b.w3(96, 160, 0)
    vmf.ent("prop_button", dict(targetname="tb_btn_pedestal", origin=fmt(pbp), angles=fmt(b.ang(0)), Delay="2", istimer="1"),
            [("OnPressed", "tb_btn_lamp2", "TurnOn", "", 0), ("OnButtonReset", "tb_btn_lamp2", "TurnOff", "", 0), ("OnPressed", "tb_cnt_pedestal", "Add", "1", 0)])
    b.record("tb_btn_pedestal", "prop_button", origin=pbp, note="timer button, 2 s")
    # (bridge #31) output counters the tests read: how often each button fired OnPressed (pcb_assert_ent_float math_counter tb_cnt_floor m_OutValue 1)
    for cn in ("tb_cnt_floor", "tb_cnt_pedestal"):
        vmf.ent("math_counter", dict(targetname=cn, origin=fmt(b.w3(0, 300, 20)), min="0", max="0", StartDisabled="0"))
    b.light(-96, 160, 200, name="tb_btn_lamp", start_off=True)
    b.light(96, 160, 200, name="tb_btn_lamp2", start_off=True)
    b.light(0, 400, 300)
    b.go = [("tb_cube_template", "ForceSpawn", "", 0), ("tb_btn_floor", "PressIn", "", 0.5), ("tb_btn_pedestal", "Press", "", 0.5)]
    b.reset = [("tb_cube", "Dissolve", "", 0), ("tb_btn_floor", "PressOut", "", 0.5)]
    b.go_expect = "a new cube appears at tb_cube's start spot and falls 224 u to the floor; both buttons are pressed, their lamps come on"
    b.reset_expect = "every cube dissolves, floor button released"

    # laser: emitter, catcher, relay and reflective cube
    b = B("laser", "W", 3, 2, 512, 256)
    hw = b.w / 2
    b.pocket("left", (216, 296, 24, 104))      # emitter pocket
    b.pocket("right", (216, 296, 24, 104))     # catcher pocket
    b.pocket("back", (-40, 40, 24, 104))
    em = b.w3(-hw - 16, 256, 64)
    vmf.ent("env_portal_laser", dict(targetname="tb_laser_emitter", origin=fmt(em), angles=fmt(b.ang(90, 0, 0)), model="models/props/laser_emitter_center.mdl", StartState="0", AutoAimEnabled="1", skin="0"))
    ca = b.w3(hw + 16, 256, 64)
    vmf.ent("prop_laser_catcher", dict(targetname="tb_laser_catcher", origin=fmt(ca), angles=fmt(b.ang(270, 0, 0)), model="models/props/laser_catcher_center.mdl"),
            [("OnPowered", "tb_laser_lamp", "TurnOn", "", 0), ("OnUnpowered", "tb_laser_lamp", "TurnOff", "", 0), ("OnPowered", "tb_cnt_catcher", "Add", "1", 0)])
    vmf.ent("prop_laser_relay", dict(targetname="tb_laser_relay", origin=fmt(b.w3(0, 511, 63)), angles=fmt(b.ang(180, 0, 0)), model="models/props/laser_receptacle.mdl"),
            [("OnPowered", "tb_cnt_relay", "Add", "1", 0)])
    for cn in ("tb_cnt_catcher", "tb_cnt_relay"):
        vmf.ent("math_counter", dict(targetname=cn, origin=fmt(b.w3(0, 40, 20)), min="0", max="0", StartDisabled="0"))
    vmf.ent("prop_weighted_cube", dict(targetname="tb_laser_cube", origin=fmt(b.w3(-64, 128, 32)), angles="0 0 0", CubeType="1", NewSkins="1", allowfunnel="1"))
    b.record("tb_laser_emitter", "env_portal_laser", origin=em, beam="+u across the bay at z=64, v=256")
    b.record("tb_laser_catcher", "prop_laser_catcher", origin=ca, note="receives the emitter's beam; lights tb_laser_lamp")
    b.record("tb_laser_relay", "prop_laser_relay", origin=b.w3(0, 511, 63), note="on the back wall; powered by a redirected beam")
    b.record("tb_laser_cube", "prop_weighted_cube", origin=b.w3(-64, 128, 32), note="reflective cube (CubeType 1): pick up and put in the beam")
    b.light(0, 300, 240, name="tb_laser_lamp", start_off=True)
    b.light(-144, 130, 240); b.light(144, 130, 240); b.light(0, 400, 240)
    b.go = [("tb_laser_emitter", "TurnOn", "", 0)]; b.reset = [("tb_laser_emitter", "TurnOff", "", 0)]
    b.go_expect = "beam from tb_laser_emitter (left wall) crosses 576 u to tb_laser_catcher; the catcher powers and tb_laser_lamp lights"
    b.reset_expect = "beam off, catcher unpowered, lamp off"

    # bridge
    b = B("bridge", "W", 5, 1, 512, 256)
    b.pit(192, 320, 192)
    pr = b.w3(-b.w / 2 + 8, 256, 40)   # (bridge #31: 8 u in front of the wall: an origin on the wall plane gave no beam)
    vmf.ent("prop_wall_projector", dict(targetname="tb_bridge", origin=fmt(pr), angles=fmt(b.ang(90, 0, 0)), StartEnabled="0"))
    b.record("tb_bridge", "prop_wall_projector", origin=pr, note="left wall, projects +u across a 128 u wide, 192 deep pit")
    b.record("tb_bridge_pit", "info_target", origin=b.w3(0, 256, -190), note="bottom of the pit the bridge spans")
    vmf.ent("info_target", {"targetname": "tb_bridge_pit", "origin": fmt(b.w3(0, 256, -190))})
    # (bridge #32) "the bridge renders as a thin line": it is a flat slab 64 u wide at z=40, and the standing spot was 160 u outside the bay with the eye at 72: the
    # slab (v 224..288, i.e. 384..448 u away) is then seen almost edge-on, 32 u below the eye: 0.67 degrees tall = about 12 px on a 1080p screen. A light bridge is
    # always a flat strip, so it has to be looked at from above and close: the spot is 84 u before the pit's rim (v 140), 74..138 u from the strip: ~10 degrees.
    b.spot_v = 140
    b.go = [("tb_bridge", "Enable", "", 0)]; b.reset = [("tb_bridge", "Disable", "", 0)]
    b.go_expect = "a light bridge crosses the pit (v 192..320, 192 deep) along the u axis at z=40 from the left wall to the right wall"
    b.reset_expect = "bridge off"
    b.light(0, 130, 240); b.light(0, 400, 240)

    # funnel
    b = B("funnel", "W", 6, 1, 512, 384)
    fp = b.w3(0, 256, 0)
    vmf.ent("prop_tractor_beam", dict(targetname="tb_funnel", origin=fmt(fp), angles="-90 0 0", linearForce="250", StartEnabled="0", use128model="1", disableautoreprojection="1"))
    b.record("tb_funnel", "prop_tractor_beam", origin=fp, note="floor emitter, beam up to the ceiling at z=384")
    b.go = [("tb_funnel", "Enable", "", 0)]; b.reset = [("tb_funnel", "Disable", "", 0)]
    b.go_expect = "excursion funnel rises from the floor at u=0, v=256 to the ceiling; entering it lifts a player or cube at 250 u/s"
    b.reset_expect = "funnel off"
    b.light(0, 130, 372); b.light(0, 400, 372)

    # ------------------------------------------------------------------------------------------ E side
    # faith plate
    b = B("faith", "E", 0, 1, 640, 384)
    # (bridge #32) Valve's faith_plate_floor instance puts the model 64 u BELOW the floor surface (origin 0 0 -64): the plate is recessed and
    # its top is flush. At z=0 the whole arm stood up out of the floor as a solid prop and Chell stopped against it at v=171.
    plate = b.w3(0, 128, -64)
    tgt = b.w3(0, 512, 0)
    vmf.ent("prop_dynamic", dict(targetname="tb_faith_arm", model="models/props_ingame/faith_plate_mapeditor.mdl", origin=fmt(plate), angles=fmt(b.ang(180)), solid="6", DefaultAnim="straightup"))
    cb = b.brush(-64, 64, 64, 192, 0, 64, "TOOLS/TOOLSTRIGGER")
    vmf.ent("trigger_catapult", dict(targetname="tb_faith", launchTarget="tb_faith_target", playerSpeed="350", physicsSpeed="350", useThresholdCheck="0", lowerThreshold="0.15", upperThreshold="0.30",
                                    StartDisabled="0", spawnflags="4105", launchDirection="0 0 0", exactVelocityChoiceType="0", useExactVelocity="0", entryAngleTolerance="0.0", applyAngularImpulse="1", AirCtrlSupressionTime="-1.0",
                                    origin="0 0 0"), solids=[cb])
    vmf.ent("info_target", {"targetname": "tb_faith_target", "origin": fmt(tgt)})
    b.record("tb_faith", "trigger_catapult", cb[0], cb[1], speed=350, target="tb_faith_target", note="(bridge #32) speed 350 and no threshold check: a standing player is launched along a 75 u high arc; Valve's maps use 600 and useThresholdCheck 1")
    b.record("tb_faith_arm", "prop_dynamic", origin=plate)
    b.record("tb_faith_target", "info_target", origin=tgt, note="landing spot, 384 u from the plate along +v")
    b.go = [("tb_faith", "Enable", "", 0), ("tb_faith_arm", "SetAnimation", "angled", 0)]
    b.reset = [("tb_faith", "Disable", "", 0), ("tb_faith_arm", "SetAnimation", "straightup", 0)]
    b.go_expect = "plate arm animates and the catapult trigger is armed: stepping on the plate launches a player or cube at 350 u/s to tb_faith_target (an arc about 75 u high)"
    b.reset_expect = "catapult disabled"
    b.light(0, 130, 372); b.light(0, 400, 372); b.light(0, 560, 372)

    # fizzler
    b = B("fizzler", "E", 1, 1, 512, 256)
    for (u0, u1) in ((-b.w / 2, -64), (64, b.w / 2)):
        b.box(u0, u1, 256, 288, 0, 256, WHITE_W)
    b.box(-64, 64, 256, 288, 128, 256, WHITE_W)
    fz = b.w3(0, 272, 64)
    cz = b.brush(-64, 64, 271.5, 272.5, 0, 128, "TOOLS/TOOLSTRIGGER")
    vmf.ent("trigger_portal_cleanser", dict(targetname="tb_fizzler", spawnflags="9", StartDisabled="0", origin="0 0 0"), solids=[cz])
    vmf.ent("prop_dynamic", dict(targetname="tb_fizzler_model", model="models/props/fizzler.mdl", origin=fmt(fz), angles=fmt(b.ang(0)), solid="0", skin="0"))   # (bridge #32) solid 6 filled the whole doorway: nobody could cross the grid; a fizzler is not solid
    vmf.ent("prop_weighted_cube", dict(targetname="tb_fizz_cube", origin=fmt(b.w3(0, 150, 32)), angles="0 0 0", NewSkins="1"))
    # (bridge #32) a copy that tb_reset_fizzler brings back, so a dissolved cube does not stay gone for the next test
    vmf.ent("point_template", dict(targetname="tb_fizz_cube_template", Template01="tb_fizz_cube", spawnflags="2", origin=fmt(b.w3(0, 150, 32))))
    b.record("tb_fizzler", "trigger_portal_cleanser", cz[0], cz[1], note="128 x 128 emancipation grid in the partition doorway at v=272")
    b.record("tb_fizzler_model", "prop_dynamic", origin=fz)
    b.record("tb_fizz_cube", "prop_weighted_cube", origin=b.w3(0, 150, 32), note="carry it into the grid to see it dissolve")
    b.go = [("tb_fizzler", "Disable", "", 0), ("tb_fizzler_model", "Disable", "", 0)]
    b.reset = [("tb_fizzler", "Enable", "", 0), ("tb_fizzler_model", "Enable", "", 0), ("tb_fizz_cube", "Kill", "", 0), ("tb_fizz_cube_template", "ForceSpawn", "", 0.3)]
    b.go_expect = "grid turns OFF (it starts on): the doorway at v=272 can be crossed by a cube or a portal"
    b.reset_expect = "grid on again: anything crossing it is fizzled"
    b.light(0, 130, 240); b.light(0, 400, 240)

    # turret
    b = B("turret", "E", 2, 1, 512, 256)
    tp = b.w3(0, 220, 0)   # (bridge #31: 400 -> 220 u deep: the turret was a speck from the standing spot)
    vmf.ent("npc_portal_turret_floor", dict(targetname="tb_turret", origin=fmt(tp), angles=fmt(b.ang(180)), spawnflags="32", TurretRange="1024", PickupEnabled="1", SkinNumber="0", AllowShootThroughPortals="0",
                                           CollisionType="0", DamageForce="1", DisableMotion="0", Gagged="0", LoadAlternativeModels="0", ModelIndex="0", UsedAsActor="0", UseSuperDamageScale="0"))
    b.record("tb_turret", "npc_portal_turret_floor", origin=tp, note="faces the bay opening, range 1024, starts inactive")
    b.go = [("tb_turret", "Enable", "", 0)]; b.reset = [("tb_turret", "Disable", "", 0)]
    b.go_expect = "turret wakes and tracks/fires at anything in its 1024 u range along its facing (toward the hall)"
    b.reset_expect = "turret sleeps"
    b.light(0, 130, 240); b.light(0, 400, 240)

    # goo
    b = B("goo", "E", 3, 1, 512, 256)
    pf = -96
    b.pit(160, 400, 96)
    b.box(-b.w / 2, b.w / 2, 160, 400, pf, -32, GOO)
    hurt = b.brush(-b.w / 2, b.w / 2, 160, 400, pf, -16, "TOOLS/TOOLSTRIGGER")
    vmf.ent("trigger_hurt", dict(targetname="tb_goo_hurt", damage="1000", damagecap="1000", damagemodel="0", damagetype="262144", nodmgforce="0", spawnflags="4097", StartDisabled="0", origin="0 0 0"), solids=[hurt])
    vmf.ent("info_target", {"targetname": "tb_goo_surface", "origin": fmt(b.w3(0, 280, -32))})
    b.record("tb_goo_hurt", "trigger_hurt", hurt[0], hurt[1], note="kills on contact; goo surface z=-32, pit v 160..400 across the whole bay width")
    b.record("tb_goo_surface", "info_target", origin=b.w3(0, 280, -32))
    b.go = [("tb_goo_hurt", "Disable", "", 0)]; b.reset = [("tb_goo_hurt", "Enable", "", 0)]
    b.go_expect = "goo stops hurting (go = harmless), reset = deadly again; the pit is 240 u deep in v, 96 u deep"
    b.reset_expect = "goo hurts again"
    b.light(0, 130, 240); b.light(0, 280, 240); b.light(0, 400, 240)

    # doors
    b = B("doors", "E", 4, 1, 512, 256)
    for (u0, u1) in ((-b.w / 2, -64), (64, b.w / 2)):
        b.box(u0, u1, 256, 288, 0, 256, WHITE_W)
    b.box(-64, 64, 256, 288, 128, 256, WHITE_W)
    dpos = b.w3(0, 272, 0)
    vmf.ent("prop_testchamber_door", dict(targetname="tb_door", origin=fmt(dpos), angles=fmt(b.ang(0))))
    b.record("tb_door", "prop_testchamber_door", origin=dpos, note="test chamber door in the partition, doorway 128 x 128")
    b.go = [("tb_door", "Open", "", 0)]; b.reset = [("tb_door", "Close", "", 0)]
    b.go_expect = "door animates open (about 1.5 s), the doorway at v=272 is passable"
    b.reset_expect = "door closes"
    b.light(0, 130, 240); b.light(0, 400, 240)

    # gel
    b = B("gel", "E", 5, 1, 512, 320)
    gp = b.w3(0, 256, 300)
    vmf.ent("info_paint_sprayer", dict(targetname="tb_gel", origin=fmt(gp), angles="90 0 0", PaintType="0", blobs_per_second="8", maxblobcount="150", min_speed="150", max_speed="150", blob_spread_radius="24",
                                      blob_spread_angle="4", blob_streak_percentage="100", min_streak_time="1", max_streak_time="1", start_active="0", RenderMode="0", AmbientSound="0"))
    b.record("tb_gel", "info_paint_sprayer", origin=gp, note="blue (bounce) gel straight down onto the floor at v=256")
    b.panel_rec("tb_gel_floor", "floor", (-64, 64, 192, 320), note="floor under the sprayer")
    b.go = [("tb_gel", "Start", "", 0)]; b.reset = [("tb_gel", "Stop", "", 0)]
    b.go_expect = "gel falls from the ceiling at u=0 v=256 (8 blobs/s, 60 u/s) and coats the floor"
    b.reset_expect = "sprayer stops"
    b.light(0, 130, 300); b.light(0, 400, 300)

    # ------------------------------------------------------------------------------------------ emit
    for b in bays:
        b.rec["main"] = MAIN[b.name]
        b.rec["settle_s"] = SETTLE.get(b.name, 3)
        b.rec["ride"] = b.name in RIDE
        b.rec["ride_moves"] = b.name in RIDE and b.name != "arrival"   # the arrival car is parked when the test starts
        if b.name in RIDE_AT:
            b.rec["ride_at"] = [round(x, 2) for x in b.w3(*RIDE_AT[b.name])]
        b.record_bay()
    return bays


class ElevatorBay(Bay):
    """arrival (rises from a shaft below) or departure (rises into a tall shaft above): Valve's own
    logic instance (elevator_arrival_logic / elevator_departure_logic) in a foyer built here."""

    def __init__(self, vmf, kind, side, slot):
        # (bridge #32) the arrival car stands 640 u in from the hall wall, as Valve's flow expects: elevator_arrival_logic opens the entry door with
        # its `leaving_elevator_trigger` 384..448 u in front of the car (local y -448..-384), so the door must be BEYOND that trigger. With the car 176 u in
        # the trigger was out in the hall (208 u past the closed door): a player arriving in the car never touched it; only a player walking up from the
        # hall did. 640 puts the trigger at v 192..256, the door 224+ u further on, like pit_flings (trigger 208 u before its door).
        cv = 640 if kind == "arrival" else 176
        super().__init__(vmf, kind, side, slot, 1, cv + 144, 256, wall=BLACK_W, floor=BLACK_F, ceil=BLACK_C, opening=(192, 160))
        self.kind = kind
        self.cu, self.cv = 0, cv
        self.shaft = (96, 96)   # half size of the shaft (u, v)
        down, up = (840, 470) if kind == "arrival" else (400, 4500)
        v0, v1 = cv - 96, cv + 96
        self.hole("floor", (-96, 96, v0, v1))
        self.hole("ceil", (-96, 96, v0, v1))
        ring = ((-96 - WT, -96, v0 - WT, v1 + WT), (96, 96 + WT, v0 - WT, v1 + WT), (-96, 96, v0 - WT, v0), (-96, 96, v1, v1 + WT))
        self.box(-96 - WT, 96 + WT, v0 - WT, v1 + WT, -down - WT, -down, BLACK_F)
        self.box(-96 - WT, 96 + WT, v0 - WT, v1 + WT, up, up + WT, BLACK_C)
        for (u0, u1, vv0, vv1) in ring:
            self.box(u0, u1, vv0, vv1, -down, 0, BLACK_W)
            self.box(u0, u1, vv0, vv1, self.H, up, BLACK_W)
        # light
        if kind == "arrival":
            for vv in (90, 290, 490):
                self.light(0, vv, 240)
            self.light(-64, cv + 74, 240)
        else:
            self.light(0, 60, 240)
            self.light(-64, 250, 240)

    def finish(self):
        vmf, b = self.vmf, self
        z0 = -2
        car = b.w3(0, self.cv, z0)
        # the instance is built facing local -y out of the car door; door must face the hall (-into)
        want = (-b.into[0], -b.into[1])
        yaw = {(0, 1): 180, (0, -1): 0, (1, 0): 90, (-1, 0): 270}[want]
        f = "instances/p2editor/elevator_%s_logic.vmf" % self.kind
        vmf.ent("func_instance", dict(targetname="%s_elevator" % self.kind, file=f, origin=fmt((car[0], car[1], z0)), angles=fmt((0, yaw, 0))))
        self.record("%s_elevator" % self.kind, "func_instance", origin=(car[0], car[1], z0), note="Valve's %s logic instance; the car is %s-elevator-elevator_1" % (self.kind, self.kind))
        # the doorway in the hall wall carries @entry_door / @exit_door
        dn = "@entry_door" if self.kind == "arrival" else "@exit_door"
        dpos = b.w3(0, -32, 0)
        vmf.ent("prop_testchamber_door", dict(targetname=dn, origin=fmt(dpos), angles=fmt(b.ang(180 if self.kind == "arrival" else 0))))   # (bridge #31: only the arrival door is turned 180 deg; the user saw it the wrong way round)
        self.record(dn, "prop_testchamber_door", origin=dpos, note="elevator door in the hall wall (opened by the arrival logic, closed by the departure logic)")
        if self.kind == "arrival":
            # (bridge #32) Valve's own trigger (arrival_elevator-leaving_elevator_trigger, v 192..256 in the foyer) opens @entry_door for a player who has
            # left the car. This one keeps the old way in from the other side: walking up to the closed door in the hall opens it too.
            hall = b.brush(-64, 64, -272, -208, 0, 128, "TOOLS/TOOLSTRIGGER")
            vmf.ent("trigger_once", dict(targetname="tb_entry_hall_trigger", spawnflags="1", StartDisabled="0", origin="0 0 0"), [("OnTrigger", "@entry_door", "Open", "", 0.5)], solids=[hall])
            self.record("tb_entry_hall_trigger", "trigger_once", hall[0], hall[1], note="in the hall, 208..272 u from the arrival doorway: opens @entry_door for a player coming from the hall")
            self.go = [("arrival_elevator-open", "Trigger", "", 0), ("arrival_elevator-signs_on", "Trigger", "", 0)]
            self.reset = [("arrival_elevator-close", "Trigger", "", 0), ("arrival_elevator-signs_off", "Trigger", "", 0)]
            self.go_expect = "arrival car door opens (open relay), signs on"
            self.reset_expect = "car door closes"
        else:
            # (bridge #32) the car is a func_tracktrain with speed 0 and startspeed 1000: StartForward gave it no real speed (it rose 20 u in 7 s).
            # Valve's own start (transitions/sp_elevator_motifs.nut StartMoving) sets SetSpeedReal 300 when no level speed is known; so does this.
            # Reset: stop, speed 0, back to the first path node, door open.
            # (bridge #39) tbtest full5: the car rose 20 u and stopped when Chell stood in it (in Minecraft mode it did not move at all), but left at 300 u/s with
            # nobody in it. The instance's own start (the logic_branch_listener's OnAllTrue, read from the vmf) is: doorclose_playerclip Enable and `close` at once;
            # one second later floor_clip Disable, `elevator_1 RunScriptCode StartMoving()` (Valve's script, which sets the speed from the map name: pc_map_alias
            # says sp_a2_pit_flings, 200 u/s) and signs_off. The old go only closed the door and set the speed: floor_clip (the static clip floor the player stands on
            # at rest, 23 u above the car's own floor) stayed enabled, so the car moved up into a player standing on a solid that did not move with it. Now the go is
            # exactly Valve's, and the reset puts all of it back (floor_clip on, the door clip off, the car stopped on its first path node, the door open).
            # NOTE the instance's `in_elevator` trigger also starts the car by itself when the player has been in the car with nothing colliding: a player placed in
            # the car can see the car leave without go, a second after placement. That is Valve's flow; tests should place and go together.
            self.go = [("departure_elevator-elevator_doorclose_playerclip", "Enable", "", 0), ("departure_elevator-close", "Trigger", "", 0),
                       ("departure_elevator-floor_clip", "Disable", "", 1.0), ("departure_elevator-elevator_1", "RunScriptCode", "StartMoving()", 1.0)]
            self.reset = [("departure_elevator-elevator_1", "SetSpeedReal", "0", 0), ("departure_elevator-elevator_1", "Stop", "", 0.02),
                          ("departure_elevator-elevator_1", "TeleportToPathNode", "departure_elevator-elevator_1_path_0", 0.1),
                          ("departure_elevator-floor_clip", "Enable", "", 0.15), ("departure_elevator-elevator_doorclose_playerclip", "Disable", "", 0.15),
                          ("departure_elevator-elevator_1", "SetSpeedReal", "0", 0.3), ("departure_elevator-elevator_1", "Stop", "", 0.32),
                          ("departure_elevator-logic_source_elevator_door_open", "Trigger", "", 0.4)]
            self.go_expect = "departure car door closes, then the car rises up the 4500 u shaft"
            self.reset_expect = "car stopped and back at the floor with its door open"
        self.rec["car"] = {"origin": list(car), "yaw": yaw, "floor_z": 0}


def main():
    out, layout = sys.argv[1], sys.argv[2]
    vmf = Vmf()
    bays = build(vmf)
    # elevator instances placed after the shell logic
    for b in bays:
        if isinstance(b, ElevatorBay):
            b.finish()
    # every bay: shell, relays
    for b in bays:
        b.relays()
    # --- hall
    H = HALL
    holes = {"S": [], "N": [], "W": [], "E": []}
    for b in bays:
        holes[b.side].append(b.opening_rect())
        b.emit()
    slab(vmf, 2, -WT, 0, (-OUT, OUT), (-OUT, OUT), True, BLACK_F)
    slab(vmf, 2, HALL_H, HALL_H + WT, (-OUT, OUT), (-OUT, OUT), False, BLACK_C)
    slab(vmf, 1, -OUT, -H, (-OUT, OUT), (-WT, HALL_H + WT), True, BLACK_W, holes["S"])
    slab(vmf, 1, H, OUT, (-OUT, OUT), (-WT, HALL_H + WT), False, BLACK_W, holes["N"])
    slab(vmf, 0, -OUT, -H, (-OUT, OUT), (-WT, HALL_H + WT), True, BLACK_W, holes["W"])
    slab(vmf, 0, H, OUT, (-OUT, OUT), (-WT, HALL_H + WT), False, BLACK_W, holes["E"])
    # hall lights: 5 x 5 grid
    for x in (-960, -480, 0, 480, 960):
        for y in (-960, -480, 0, 480, 960):
            vmf.ent("light", {"origin": "%d %d %d" % (x, y, HALL_H - 24), "_light": "%s %d" % (LIGHT_RGB, HALL_B), "_lightHDR": "-1 -1 -1 1", "_lightscaleHDR": "1", "_constant_attn": "0",
                              "_linear_attn": "0", "_quadratic_attn": "1", "_fifty_percent_distance": str(HALL_NEAR), "_zero_percent_distance": str(HALL_FAR), "_hardfalloff": "0", "style": "0", "spawnflags": "0"})
    vmf.ent("env_cubemap", {"origin": "0 0 128", "cubemapsize": "0"})
    vmf.ent("light_environment", {"origin": "0 0 300", "pitch": "-85", "angles": "0 290 0", "SunSpreadAngle": "10", "_light": "133 156 182 300", "_lightHDR": "-1 -1 -1 1", "_lightscaleHDR": "1",
                                  "_ambient": "133 156 182 300", "_ambientHDR": "-1 -1 -1 1", "_AmbientScaleHDR": "1"})
    # player start: in the middle of the hall; the arrival logic's own flow is tested with tb_go_arrival
    arr = [b for b in bays if b.name == "arrival"][0]
    sp = (arr.rec["car"]["origin"][0], arr.rec["car"]["origin"][1], arr.rec["car"]["origin"][2] - 648)
    vmf.ent("info_player_start", {"origin": fmt(sp), "angles": fmt((0, (arr.yaw + 180) % 360, 0))})
    LAY_START["origin"] = list(sp)
    # Valve's global_ents (@command, @clientcommand, @tonemapper, the environment_* relays) in a void box, then the
    # same environment pit_flings runs: environment_testchamber (fog 40 53 64, tonemap min .5 max 3, rate .25)
    vmf.ent("func_instance", {"targetname": "global_ents", "file": "instances/p2editor/global_ents.vmf", "origin": "3200 -3200 0", "angles": "0 0 0"})
    vmf.ent("logic_auto", {"spawnflags": "1", "origin": "0 0 48"}, [("OnMapSpawn", "global_ents-environment_testchamber", "Trigger", "", 0.5), ("OnMapSpawn", "tb_bridge", "Disable", "", 0.3), ("OnMapSpawn", "tb_funnel", "Disable", "", 0.3),
                                                                  # (bridge #32) Chell's dual portal gun, the way Valve's flow gives it: transitions/sp_transition_list.nut (OnPostTransition) sends the
                                                                  # console commands give_portalgun and upgrade_portalgun to `@command` (global_ents' point_servercommand). That script is not in
                                                                  # this map (and pc_map_alias only changes what GetMapName() says), so the same two commands go out here, once the player exists.
                                                                  # (bridge #34) ONCE, as Valve does it (give at 0.0, upgrade at 0.1, never repeated). The repeat at 6.0 s / 6.1 s that #32 added re-gave the gun
                                                                  # in the middle of tbtest's first shots (full3: ~6 s after the map started): the fresh gun is deploying, so the blue shot was swallowed.
                                                                  ("OnMapSpawn", "@command", "Command", "give_portalgun", 1.5), ("OnMapSpawn", "@command", "Command", "upgrade_portalgun", 1.6)])   # (bridge #31) the projectors start off whatever StartEnabled does
    vmf.ent("shadow_control", {"origin": "0 0 40", "angles": "90 0 0", "color": "98 102 106", "distance": "100", "enableshadowsfromlocallights": "1", "disableallshadows": "0"})
    # map-name alias (Valve's scripts choose music, videos and the next map from GetMapName)
    alias = ["entity", "{", '\t"id" "%d"' % vmf.ids(), '\t"classname" "logic_script"', '\t"targetname" "pc_map_alias"', '\t"vscripts" "portalcraft/pc_map_alias.nut"', '\t"origin" "0 0 0"', "}"]
    # global ents (Valve's, by instance): @glados is not provided; @command/@tonemapper above
    vmf.ents = alias + vmf.ents
    # Minecraft region (same box as pc_pit_flings)
    rids = Ids(10_000_000)
    region = add_region.region_solids(rids)
    rents = add_region.region_markers(rids)
    lines = ["versioninfo", "{", '\t"editorversion" "400"', '\t"editorbuild" "5633"', '\t"mapversion" "1"', '\t"formatversion" "100"', '\t"prefab" "0"', "}", "visgroups", "{", "}",
             "world", "{", '\t"id" "1"', '\t"mapversion" "1"', '\t"classname" "worldspawn"', '\t"skyname" "sky_white"', '\t"detailmaterial" "detail/detailsprites"',
             '\t"detailvbsp" "detail.vbsp"', '\t"maxblobcount" "250"', '\t"maxpropscreenwidth" "-1"', '\t"paintinmap" "1"']
    lines += vmf.solids + region + ["}"] + vmf.ents + rents
    open(out, "w", encoding="latin-1").write("\n".join(lines) + "\n")
    # layout json
    data = {"map": "pc_testbed", "units": "Source units; z=0 is every floor top (except pits); u runs along each bay's width, v into the bay",
            "hall": {"min": [-HALL, -HALL, 0], "max": [HALL, HALL, HALL_H]},
            "start": LAY_START,
            "region": {"min": list(add_region.REGION_MIN), "max": list(add_region.REGION_MAX)},
            "order": LAY.order, "bays": LAY.bays}
    json.dump(data, open(layout, "w"), indent=1)
    print("bays:", ", ".join(LAY.order), "| solids:", vmf.solids.count("\tsolid"), "entities:", vmf.ents.count("entity"))


if __name__ == "__main__":
    main()
