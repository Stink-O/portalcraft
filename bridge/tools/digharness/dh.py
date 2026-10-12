#!/usr/bin/env python3
"""Dig-render harness (bridge #73, DESIGN-dig-render.md section 4): scripted reproductions of the user's six digging symptoms,
judged from data the plugin records (src/bridge/digharness.cpp), not from screenshots.

  tools/digharness/dh.py run <tag> --so /abs/plugin.so [--map pc_testbed] [--pre 'pc_dig_render 2'] [--scen s30,s31,s33,s35]
  tools/digharness/dh.py judge <tag>
  tools/digharness/dh.py script <tag> [...]          (write the game script only)

Each probe (pc_dh_probe) writes cache/bridge/digharness/<tag>/<probe>.json: for a 64 x 36 grid of pixels, the expected first
hit (Portal 2's world with the dug cells removed, the Minecraft blocks, entities, water), the old wall's hit, and what the frame
drew there (depth -> distance, colour), plus every networked entity's client state and the camera's leaf.

Scenarios (map-agnostic: walls are found by tracing from the player, cuts are mined through Minecraft as a player would):
  s30  a 1 x 2 x 2 cut at a wall's foot, seen from 3 blocks: ghosts of the wall over the cut (depth class + A/B colour vs the
       same pose before digging), z-fighting (two frames)
  s35  the same cut from 2, 5, 10, 20 blocks (34 = close, 35 = far): ghost / void per distance
  s32  standing in the room in front of the cut, facing away from it (the reference for s33)
  s33  standing INSIDE the cut, same direction: entities the client drops (dormant), water
  s31  a cut through a wall that has open air behind it, looked through from the front and at an angle: void, ghost, z-fight
  seam (bridge #76) a 1 x 2 x 3 tunnel; rays aimed at its seams (the faces between dug cells and the cells the host's face runs
       through) from 6 poses outside and 3 inside: void (the clear colour) or missing surface fail
"""
import argparse
import json
import math
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
OUT = os.path.join(ROOT, "cache", "bridge", "digharness")

# Where each map's scenario starts: (feet x y z, yaw toward the wall to dig, yaw toward a wall with a room behind it).
# pc_testbed: the east hall wall at y 1000 has no bay behind it (E slot 6 is free); the west wall faces the bays' rooms.
START = {
    # the east hall wall at y 1000 has no bay behind it (E slot 6 is free); the west wall faces the bays' rooms
    "pc_testbed": {"feet": (1000.0, 1000.0, 8.0), "yaw": 0, "aim": "0 40 600", "through_feet": (-900.0, 300.0, 8.0), "through_yaw": 180, "through_aim": "180 60 900 1 130"},
    # campaign chambers: next to a floor button (the entity lumps), then the nearest white wall all round
    # (from run pre2's probes: the white tile wall ~180 u north of this spot is in clear view; the thin plastic panel at y 448 has the
    # button room behind it)
    # (bridge #75) s30: the chamber's west white wall (x -704, seen in run e5i1's probes): the first spot (the wall north of the start)
    # was behind hanging vines and the button's platform, half of its silhouette hidden
    "sp_a1_intro2": {"feet": (-560.0, 100.0, -56.0), "yaw": 180, "aim": "180 60 400 0 200 white", "through_feet": (-446.7, 288.0, 0.0), "through_yaw": 90, "through_aim": "90 60 400 1 130 -"},
    # (bridge #75) water: the goo pit (BSP water brush x 2880..3584, y -1792..-768, top z -96; floor beside it z 32), the west wall
    # 136 u from this spot (found offline from the BSP's leaves); the s33 water probes look back out over the goo
    "sp_a2_laser_over_goo": {"feet": (2820.0, -1700.0, 40.0), "yaw": 180, "aim": "180 60 400 0 200 -", "through_feet": (3424.0, -1848.0, 40.0), "through_yaw": 180, "through_aim": "180 60 200 1 130 -"},  # a 62 u wall 100 u west (BSP leaves)
    "sp_a1_intro4": {"feet": (400.0, -400.0, 30.0), "yaw": 0, "aim": "-999 120 700 0 200 white", "through_feet": (400.0, -400.0, 30.0), "through_yaw": 0, "through_aim": "-999 100 900 1 130 -"},
}


# (bridge #76) the user's own cut of 38_seam_void.png (sp_a2_laser_over_goo, from mc.log of the session: MC cells x 65..67, y 1..2, z 24, a
# slot along the chamber's wall whose face runs 8 u into the cells; offline `colharvest --caps` finds a 2 u gap at its west end, floor and
# ceiling) and poses in the room south of it (feet, yaw, pitch) and inside it (Minecraft /tp, yaw, pitch)
USER38 = {
    "sp_a2_laser_over_goo": {
        "cells": [(x, y, 24) for x in (65, 66, 67) for y in (1, 2)],
        "out": [("u_front", (2640.0, -1070.0, 41.0), 90, 5), ("u_westend", (2700.0, -1050.0, 41.0), 130, 5), ("u_close", (2665.0, -1028.0, 41.0), 150, 12),
                ("u_low", (2640.0, -1045.0, 41.0), 100, 35), ("u_high", (2640.0, -1045.0, 41.0), 100, -30)],
        "in": [("u_in_west", (66.5, 1.02, 24.3), 180, 0), ("u_in_west_up", (66.5, 1.02, 24.3), 170, -20), ("u_in_east", (65.6, 1.02, 24.3), 0, 10)],
    },
}


def frames(s):
    return "wait %d" % max(1, int(round(s * 60)))


def build(tag, mp, scen, mode, pre=""):
    st = START.get(mp)
    L = ["# dig-render harness run %s (tools/digharness), generated" % tag, "sv_cheats 1", "pcb_assert_soft 1", "pc_input_lock 1",
         frames(2), "waituntil link 3600", frames(2)]
    if mode == "mc":
        L += ["bridge_mode mc", "waituntil mcmode 900", "waituntil mcalive 600", "pcb_mc_cmd gamemode creative", frames(1)]
    else:
        L += ["bridge_mode chell", "waituntil chellmode 900", frames(1)]
    L += ["pc_dig 1", "pcb_dig_reset", frames(2)]

    def place(p, yaw):
        if mode == "mc":
            return ["pcb_mc_place %.1f %.1f %.1f %d 24" % (p[0], p[1], p[2], yaw), "waituntil mcplaced 900", frames(0.5), "pcb_setview 8 %d" % yaw]
        return ["setpos %.1f %.1f %.1f" % p, "setang 8 %d 0" % yaw, frames(0.6)]

    def settle(s=1.2):
        return (["waituntil mcplaced 900"] if mode == "mc" else []) + [frames(s)]

    def walk_in(pitch):
        # (the placement refuses a spot inside Portal 2's solid, which a dug cell still is for the engine: walk in, as a player would)
        key = "pcb_mckey 26 %d" if mode == "mc" else ("+forward" if True else "")
        if mode == "mc":
            return ["pc_dh_standoff 24 %d 0" % pitch] + settle(0.8) + ["pcb_mckey 26 1", frames(2.0), "pcb_mckey 26 0", frames(0.5), "pc_dh_status"]
        return ["pc_dh_standoff 30 %d 0" % pitch] + settle(0.8) + ["+forward", frames(1.0), "-forward", frames(0.5)]

    def enter(depth=-1):
        # (bridge #75) into the cut by Minecraft's own /tp onto its floor (a walk-in got stuck at the lip), then say where we are: the
        # judge BLOCKs s33 when the probe's eye is not in a dug cell
        if mode == "mc":
            return ["pc_dh_enter %d" % depth, frames(2.0), "pc_dh_where"]
        return walk_in(0)

    def probe(name):
        return ["pc_dh_probe %s %s" % (tag, name), frames(1.5)]

    if st is None:  # any other map: start where the player stands, dig the nearest white wall all round
        st = {"feet": None, "yaw": 0, "aim": "-999 120 700 0 200 white", "through_feet": None, "through_yaw": 0, "through_aim": "-999 100 900 1 130 -"}
    if any(s in scen for s in ("s30", "s35", "s32", "s33")):
        if st["feet"]:
            L += place(st["feet"], st["yaw"])
        L += ["pc_dh_aimwall %s" % st["aim"], "pc_dh_standoff 120 8"] + settle()
        L += probe("s30_before")
        if "perf" in scen:
            L += ["pcb_prof reset", "pc_cut_perf 200 nodig_front", "wait 480", "pcb_prof print"]
        L += ["pc_dh_wallcut 1 2 2", frames(6)]
        L += ["pc_dh_standoff 120 8"] + settle(1.5) + probe("s30_cut") + [frames(0.2)] + probe("s30_cut_b")
        L += ["pc_cut_audit 1", frames(0.3)] + probe("s30_audit") + ["pc_cut_audit 0", "pc_dig_render", "pc_cut_vis", "pc_cut_dump"]
        if "perf" in scen:  # (bridge #75) GPU time of the world draws and our CPU scopes at this pose, in both paths (CUT PERF / RUN PROF lines)
            m = 1 if "pc_dig_render 1" in pre else 2  # (end in the mode the run is about)
            for r in (m, 3 - m, m, 3 - m, m):  # interleaved A/B: an unfocused window's GPU clocks wander
                L += ["pc_dig_render %d" % r, frames(1.0), "pcb_prof reset", "pc_cut_perf 200 cut_front_m%d" % r, "wait 480", "pcb_prof print"]
        if "s35" in scen:
            for d in (80, 200, 400, 800):
                L += ["pc_dh_standoff %d 4" % d] + settle() + probe("s35_d%d" % d)
        if "s32" in scen or "s33" in scen:
            L += ["pc_dh_standoff 30 0 180"] + settle() + probe("s32_open")
            L += enter(-1) + ["pcb_setview 0 $back"] + [frames(1.0), "pc_dh_where"] + probe("s33_inside")
            if "perf" in scen:
                m = 1 if "pc_dig_render 1" in pre else 2
                for r in (m, 3 - m, m, 3 - m, m):
                    L += ["pc_dig_render %d" % r, frames(1.0), "pcb_prof reset", "pc_cut_perf 200 inside_m%d" % r, "wait 480", "pcb_prof print"] + [frames(0.3)] + probe("s33_inside_b")
            L += ["pcb_setview 20 $back150"] + [frames(0.8)] + probe("s33_inside_c")
            # (bridge #75) water from inside the cut against the same look from the room: both look back out, pitched down 15 deg
            L += ["pc_dh_face 15 180", frames(0.8)] + probe("s33_water")
            L += ["pc_dh_standoff 30 15 180"] + settle() + probe("s32_water") + ["pc_vis_clamp", "pc_cut_vis"]
    if "seam" in scen:
        # (bridge #76) the seams of a tunnel: a 1 x 2 x 3 cut into the wall, then rays aimed at the faces between the dug cells and the
        # cells the wall's face runs through (pc_dh_seam), from the front, both sides, under and over, and from inside looking back out
        if st["feet"]:
            L += place(st["feet"], st["yaw"])
        L += ["pc_dh_aimwall %s" % st["aim"], "pc_dh_standoff 120 8"] + settle()
        L += ["pc_dh_wallcut 1 2 3", frames(6), "pc_dig_caps"]
        # (bridge #79) "lightab": the same poses again with the cuts' light off (pc_cut_light 0, the old lighting), names + "_L0"
        for sfx in ([""] + (["_L0"] if "lightab" in scen else [])):
            if sfx:
                L += ["pc_cut_light 0", frames(1.5)]
            for name, pose in (("seam_front", "50 8"), ("seam_left", "45 8 30 -26"), ("seam_right", "45 8 -30 26"), ("seam_up", "40 -25"), ("seam_down", "40 35"),
                               ("seam_far_left", "120 4 25 -56")):
                L += ["pc_dh_standoff %s" % pose] + settle(1.2) + ["pc_dh_seam %s %s%s" % (tag, name, sfx), frames(2.5)]
            L += enter(-1) + ["pc_dh_face 0 170", frames(1.0), "pc_dh_where", "pc_dh_seam %s seam_inside_out%s" % (tag, sfx), frames(2.5)]
            L += ["pc_dh_face 12 150", frames(1.0), "pc_dh_seam %s seam_inside_out2%s" % (tag, sfx), frames(2.5)]
            L += ["pc_dh_face -12 205", frames(1.0), "pc_dh_seam %s seam_inside_out3%s" % (tag, sfx), frames(2.5), "pc_dig_caps", "pc_cut_light"]
            if sfx:
                L += ["pc_cut_light 1", frames(1.5)]
        if "albedo" in scen:
            # (bridge #80) the same outside poses with every surface's light taken out: Portal 2's world fullbright (its texture alone) and the
            # band unlit (the atlas texel x Minecraft's tint/AO). Per light pair, lit / albedo on each side = the light each side is drawn with.
            L += ["mat_fullbright 1", "pcb_terrain_exposure 1 1", "pcb_band_light 2", frames(2.0)]
            for name, pose in (("seam_front", "50 8"), ("seam_left", "45 8 30 -26"), ("seam_right", "45 8 -30 26"), ("seam_up", "40 -25"), ("seam_down", "40 35")):
                L += ["pc_dh_standoff %s" % pose] + settle(1.2) + ["pc_dh_seam %s %s_AL" % (tag, name), frames(2.5)]
            L += ["mat_fullbright 0", "pcb_terrain_exposure 1 0", "pcb_band_light 2", frames(1.5)]
    if "user38" in scen and mp in USER38:
        u = USER38[mp]
        L += place(u["out"][0][1], u["out"][0][2]) + settle()
        for c in u["cells"]:
            L += ["pcb_mc_cmd pcdig mine %d %d %d" % c, frames(0.3)]
        L += [frames(4), "pc_dig_caps", "pc_cut_light"] + ["pc_dig_caps_at %d %d %d" % c for c in u["cells"]]
        for sfx in ([""] + (["_L0"] if "lightab" in scen else [])):
            if sfx:
                L += ["pc_cut_light 0", frames(1.5)]
            for name, feet, yaw, pitch in u["out"]:
                L += place(feet, yaw) + settle(1.2) + ["pcb_setview %d %d" % (pitch, yaw), frames(0.8), "pc_dh_seam %s %s%s" % (tag, name, sfx), frames(2.5)]
                if name == "u_westend" and not sfx:
                    # (bridge #79) what is drawn where the cap beside the door frame should be: the band unlit (full-bright textures), then the
                    # cut-away shaders' audit (Portal 2 fragments inside dug cells painted magenta)
                    L += ["pcb_terrain_exposure 1 1", "pcb_band_light 2", frames(1.5), "pc_dh_seam %s u_westend_unlit" % tag, frames(2.5),
                          "pcb_terrain_exposure 1 0", "pcb_band_light 2", frames(1.5), "pc_cut_audit 1", frames(0.5), "pc_dh_seam %s u_westend_audit" % tag, frames(2.5), "pc_cut_audit 0", frames(0.5)]
                    # (bridge #80) the caps' atlas rect chosen as bridge #76 did (the last of the mod's wall quads seen, no alpha check)
                    # (bridge #80) the final triangles on the failing caps' faces (67,y,24 toward 67,y,23)
                    L += ["pc_dig_caps_trace 67 1 24 2", frames(1.5), "pc_dig_caps_trace 67 2 24 2", frames(1.5)]
                    L += ["pc_dig_caps_rect 0", frames(1.5)] + ["pc_dig_caps_at %d %d %d" % c for c in u["cells"]] + ["pc_dh_seam %s u_westend_rect0" % tag, frames(2.5),
                          "pc_dig_caps_rect 1", frames(1.5)]
            for name, mc, yaw, pitch in u["in"]:
                L += ["pcb_mc_cmd tp @a %.2f %.2f %.2f" % mc, frames(2.0), "pcb_setview %d %d" % (pitch, yaw), frames(0.8), "pc_dh_where", "pc_dh_seam %s %s%s" % (tag, name, sfx), frames(2.5)]
            if sfx:
                L += ["pc_cut_light 1", frames(1.5)]
        L += ["pc_dig_caps", "pc_cut_light"] + ["pc_dig_caps_at %d %d %d" % c for c in u["cells"]]
    if "s31" in scen:
        if st["through_feet"]:
            L += place(st["through_feet"], st["through_yaw"])
        L += ["pc_dh_aimwall %s" % st["through_aim"], "pc_dh_standoff 160 4"] + settle()
        L += probe("s31_before")
        L += ["pc_dh_wallcut 2 2 0", frames(6)]
        L += ["pc_dh_standoff 160 4"] + settle(1.5) + probe("s31_front") + [frames(0.2)] + probe("s31_front_b")
        L += ["pc_dh_standoff 140 4 -35 90"] + settle(1.2) + probe("s31_angle") + [frames(0.2)] + probe("s31_angle_b")
        L += enter(-1) + ["pc_dh_face 0 0", frames(1.0), "pc_dh_where"] + probe("s31_inside_through")
    L += [frames(1), "pc_dh_status", "pcb_dig_status", "quit"]
    # "$yaw": the script language has no variables; the current view yaw is what aimwall scans around
    return [l.replace("pcb_setview 0 $back", "pc_dh_face 0 180").replace("pcb_setview 20 $back150", "pc_dh_face 20 150") for l in L]


# ---------------------------------------------------------------------------------------------------------------------- judge
def load(tag, name):
    p = os.path.join(OUT, tag, name + ".json")
    if not os.path.exists(p):
        return None
    try:
        return json.load(open(p))
    except Exception as e:  # a truncated file from a crashed run
        return {"error": str(e)}


def manifest(tag):
    try:
        return json.load(open(os.path.join(OUT, tag, "manifest.json")))
    except Exception:
        return {}


def frac(a, b):
    return (a / b) if b else 0.0


def sil_stats(pr, ref=None, ghost_rgb=18):
    """Over the cut's silhouette (pixels where the old wall lies in front of what should show): depth classes, and the A/B colour
    ghost against a probe of the same pose before digging (a pixel whose colour still matches the intact wall's)."""
    S = pr["samples"]
    sil = [i for i, s in enumerate(S) if s["uncut"][0] == "host" and s["uncut"][1] >= 0 and (s["expD"] < 0 or s["uncut"][1] < s["expD"] - max(1.5, 0.006 * s["expD"]))]
    c = {}
    for i in sil:
        c[S[i]["cls"]] = c.get(S[i]["cls"], 0) + 1
    cghost = None
    if ref and "samples" in ref and len(ref["samples"]) == len(S):
        R = ref["samples"]
        same = lit = 0
        for i in sil:
            if max(R[i]["rgb"]) < 40:  # too dark to tell a ghost from a dark hole
                continue
            lit += 1
            d = max(abs(S[i]["rgb"][k] - R[i]["rgb"][k]) for k in range(3))
            if d <= max(ghost_rgb, 0.12 * max(R[i]["rgb"])):
                same += 1
        cghost = frac(same, lit) if lit >= 10 else None
    return len(sil), c, cghost


def flicker(a, b, thr=40):
    """Pixels whose colour changes between two frames of a still camera (z-fighting), among host / Minecraft pixels."""
    if not a or not b or "samples" not in a or "samples" not in b or len(a["samples"]) != len(b["samples"]):
        return None, 0
    n = ch = 0
    for s, t in zip(a["samples"], b["samples"]):
        if s["exp"] not in ("host", "mc", "mccover") or s["cls"] != "ok":
            continue
        n += 1
        if max(abs(s["rgb"][k] - t["rgb"][k]) for k in range(3)) > thr:
            ch += 1
    return frac(ch, n), n


def in_frustum(pr, o):
    m = pr["clip"]
    v = [o[0], o[1], o[2], 1.0]
    c = [sum(m[r * 4 + k] * v[k] for k in range(4)) for r in range(4)]
    if c[3] <= 1:
        return False
    return abs(c[0] / c[3]) < 0.95 and abs(c[1] / c[3]) < 0.95


def ents_lost(open_pr, in_pr):
    """Entities the client has (not dormant) in the open pose, inside the inside-pose frustum, that the client lost inside."""
    if not open_pr or not in_pr or "entities" not in open_pr or "entities" not in in_pr:
        return None, []
    inside = {e["i"]: e for e in in_pr["entities"]}
    lost, kept = [], 0
    for e in open_pr["entities"]:
        if e["client"] != 0 or not in_frustum(in_pr, e["o"]):
            continue
        f = inside.get(e["i"])
        if f is None or f["client"] != 0:
            lost.append("%s:%s#%d" % (e["cls"], e["name"], e["i"]))
        else:
            kept += 1
    return kept, lost


def verdict(ok):
    return "PASS" if ok else "FAIL"


def judge(tag):
    res = []

    def add(sid, title, ok, msg, data=None):
        res.append({"id": sid, "title": title, "verdict": ok if isinstance(ok, str) else verdict(ok), "msg": msg, "data": data or {}})

    before = load(tag, "s30_before")
    cut = load(tag, "s30_cut")
    cut_b = load(tag, "s30_cut_b")
    if cut and "samples" in cut:
        n, c, cg = sil_stats(cut, before)
        ghost = frac(c.get("ghost_wall", 0), n)
        void = frac(c.get("void", 0), n)
        fl, fn = flicker(cut, cut_b)
        okn = c.get("ok", 0)
        seen = c.get("ok", 0) + c.get("ghost_wall", 0) + c.get("void", 0)
        add("s30", "a cut at a wall's foot from 3 blocks: no ghost of the wall over it",
            n >= 10 and ghost <= 0.01 and (cg is None or cg <= 0.01) and void <= 0.01,
            "silhouette %d samples: ok %d, ghost_wall %.1f%%, void %.1f%%, colour unchanged from the intact wall %s; classes %s; flicker %s over %d" %
            (n, okn, 100 * ghost, 100 * void, "%.1f%%" % (100 * cg) if cg is not None else "n/a", c, "%.2f%%" % (100 * fl) if fl is not None else "n/a", fn),
            {"sil": n, "classes": c, "colour_ghost": cg, "flicker": fl})
        if n < 10 or seen < 0.5 * n:
            res[-1]["verdict"] = "BLOCKED"
            res[-1]["msg"] = "the cut's silhouette has %d samples, %d of them showing the cut, the wall or the void (was the cut mined? is the view blocked by props? dug=%s): " % (n, seen, cut.get("dug")) + res[-1]["msg"]
    else:
        add("s30", "a cut at a wall's foot from 3 blocks", "NO TRACE", "no s30_cut probe")
    # coverage of the cut-away shaders (pc_cut_audit: cut fragments are painted magenta instead of removed): over the silhouette,
    # every pixel that still shows the old wall's depth must be magenta, else some world draw path is not cut
    au = load(tag, "s30_audit")
    if au and "samples" in au and cut and "samples" in cut:
        S = au["samples"]
        sil = [s for s in S if s["uncut"][0] == "host" and s["uncut"][1] >= 0 and (s["expD"] < 0 or s["uncut"][1] < s["expD"] - max(1.5, 0.006 * s["expD"]))]
        wall = [s for s in sil if abs(s["act"] - s["uncut"][1]) < max(1.5, 0.006 * s["uncut"][1])]
        mag = [s for s in wall if s["rgb"][1] < 0.5 * min(s["rgb"][0], s["rgb"][2]) and min(s["rgb"][0], s["rgb"][2]) > 60]
        if wall and not mag and "pc_dig_render 2" not in (manifest(tag).get("pre") or ""):
            add("cover", "audit", "INFO", "no magenta: the cut-away shaders are off (pc_dig_render 1)")
        elif wall:
            add("cover", "audit: every world fragment inside the cut is reached by the cut-away shaders", len(mag) == len(wall), "%d of %d silhouette pixels show the wall's depth in audit mode; %d of them magenta" % (len(wall), len(sil), len(mag)))
        else:
            add("cover", "audit", "INFO", "no wall-depth pixel in audit mode over %d silhouette pixels (the cut-away shaders are off: pc_dig_render 1?)" % len(sil))
    # s35: distance
    rows = []
    worst = 0.0
    any_d = False
    for d in (80, 200, 400, 800):
        pr = load(tag, "s35_d%d" % d)
        if not pr or "samples" not in pr:
            continue
        any_d = True
        n, c, _ = sil_stats(pr)
        bad = frac(c.get("ghost_wall", 0) + c.get("void", 0), n)
        worst = max(worst, bad if n >= 4 else 0)
        rows.append("%d u (%d blocks): sil %d, ok %d, ghost %d, void %d" % (d, d // 40, n, c.get("ok", 0), c.get("ghost_wall", 0), c.get("void", 0)))
    if any_d:
        add("s35", "the same cut from 2, 5, 10, 20 blocks: the wall never comes back", worst <= 0.01, "; ".join(rows) + "; worst ghost+void %.1f%%" % (100 * worst))
    else:
        add("s35", "the same cut at distances", "NO TRACE", "no s35 probes")
    # s32 / s33: entities and water from inside the cut
    op = load(tag, "s32_open")
    ins = load(tag, "s33_inside")
    if op and ins and "entities" in op and "entities" in ins:
        kept, lost = ents_lost(op, ins)
        w_open = op["summary"]["exp_water"]
        w_in_ok = ins["summary"]["water_ok"]
        w_in_exp = ins["summary"]["exp_water"]
        cam = ins.get("camera", {})
        ok = not lost and (w_in_exp == 0 or w_in_ok >= 0.9 * w_in_exp)
        if not (cam.get("dug") or cam.get("contents", 0) & 1 or cam.get("cluster", 0) < 0):
            ok = "BLOCKED"  # the camera never got inside the cut
        add("s33", "standing inside the cut: every entity seen from the room is still there, and the water",
            ok, "kept %d, LOST %d %s; water samples ok %d of %d expected (open pose expected %d); camera inside: contents 0x%x cluster %d area %d dug %s; vis origins %d; inside samples %s" %
            (kept or 0, len(lost), lost[:12], w_in_ok, w_in_exp, w_open, cam.get("contents", 0), cam.get("cluster", -9), cam.get("area", -9), cam.get("dug"), len(cam.get("visOrigins", [])),
             ins["summary"]), {"lost": lost})
        add("s32", "standing in the room (reference)", "INFO", "entities live %d dormant %d; summary %s" % (op["summary"]["ents_live"], op["summary"]["ents_dormant"], op["summary"]))
    else:
        add("s33", "standing inside the cut", "NO TRACE", "missing s32_open / s33_inside")
    ow, iw = load(tag, "s32_water"), load(tag, "s33_water")
    if ow and iw and "summary" in ow and "summary" in iw:
        eo, ei, ki = ow["summary"]["exp_water"], iw["summary"]["exp_water"], iw["summary"]["water_ok"]
        cam = iw.get("camera", {})
        if not cam.get("dug"):
            v = "BLOCKED"
        elif eo == 0 and ei == 0:
            v = "INFO"
        else:
            v = verdict(ki >= 0.9 * ei and ei > 0)
        add("s33w", "the water seen from inside the cut (looking back out, pitched down)", v,
            "inside: %d of %d expected water samples drawn at the water's depth (from the room: %d of %d); camera dug %s cluster %s; inside ents live %d dormant %d" %
            (ki, ei, ow["summary"]["water_ok"], eo, cam.get("dug"), cam.get("cluster"), iw["summary"]["ents_live"], iw["summary"]["ents_dormant"]))
    # s31: through a wall with open air behind
    for name, title in (("s31_front", "looking through a cut into the room behind it (front)"), ("s31_angle", "the same at an angle")):
        pr = load(tag, name)
        prb = load(tag, name + "_b")
        if not pr or "samples" not in pr:
            add(name, title, "NO TRACE", "no probe")
            continue
        n, c, _ = sil_stats(pr)
        S = pr["samples"]
        exp_any = [s for s in S if s["exp"] != "none"]
        void_all = frac(sum(1 for s in exp_any if s["cls"] == "void"), len(exp_any))
        fl, fn = flicker(pr, prb)
        ghost = frac(c.get("ghost_wall", 0), n)
        voids = frac(c.get("void", 0), n)
        ok = n >= 6 and ghost <= 0.01 and voids <= 0.01 and void_all <= 0.005 and (fl is None or fl <= 0.002)
        add(name, title, ok if n >= 6 else "BLOCKED", "silhouette %d: classes %s; void in the frame %.2f%%; flicker %s over %d; summary %s" %
            (n, c, 100 * void_all, "%.2f%%" % (100 * fl) if fl is not None else "n/a", fn, pr["summary"]))
    pr = load(tag, "s31_inside_through")
    if pr and "samples" in pr:
        S = pr["samples"]
        exp_any = [s for s in S if s["exp"] != "none"]
        void_all = frac(sum(1 for s in exp_any if s["cls"] == "void"), len(exp_any))
        cam = pr.get("camera", {})
        add("s31_inside", "standing in the through-cut, looking into the room behind it", (void_all <= 0.005) if cam.get("dug") else "BLOCKED", "void %.2f%%; summary %s; camera %s" % (100 * void_all, pr["summary"], pr.get("camera")))
    # (bridge #76) seams: no void and no missing surface where a dug cell meets the cells the host's face runs through
    base = ["seam_front", "seam_left", "seam_right", "seam_up", "seam_down", "seam_far_left", "seam_inside_out", "seam_inside_out2", "seam_inside_out3"]
    for u in USER38.values():
        base += [p[0] for p in u["out"]] + [p[0] for p in u["in"]]
    for sfx in ("", "_L0"):
        judge_seams(add, res, tag, [b + sfx for b in base], sfx)
    judge_light_albedo(add, tag)
    d = os.path.join(OUT, tag)
    os.makedirs(d, exist_ok=True)
    json.dump(res, open(os.path.join(d, "results.json"), "w"), indent=1)
    with open(os.path.join(d, "report.txt"), "w") as f:
        for r in res:
            line = "%-8s %-12s %s\n         %s\n" % (r["verdict"], r["id"], r["title"], r["msg"])
            f.write(line)
            print(line, end="")
    return res


def judge_light_albedo(add, tag):
    # (bridge #80) the light each side of the mouth's edge is DRAWN with: per light pair, the lit pixel's linear luminance over the same pixel
    # with every surface's light taken out (the "_AL" probes of --scen albedo: Portal 2 fullbright, the band unlit). The pixel pairs of
    # "light" mix that with the textures' albedo (a white_concrete cap beside a grimy Portal 2 panel), this does not. Within one stop.
    def lin(v):
        v /= 255.0
        return v / 12.92 if v <= 0.04045 else ((v + 0.055) / 1.055) ** 2.4

    def pairs(pr):
        out = {}
        for s in pr.get("samples", []):
            if s.get("lp", -1) < 0 or s.get("cls") != "ok":
                continue
            c = s["rgb"]
            out.setdefault(s["lp"], {})[s["side"]] = 0.2126 * lin(c[0]) + 0.7152 * lin(c[1]) + 0.0722 * lin(c[2])
        return out
    rw, rc, rows = [], [], []
    for name in ("seam_front", "seam_left", "seam_right", "seam_up", "seam_down"):
        lit, al = load(tag, name), load(tag, name + "_AL")
        if not lit or not al or "samples" not in lit or "samples" not in al:
            continue
        L, A = pairs(lit), pairs(al)
        n0 = len(rw)
        for k, sides in L.items():
            a = A.get(k, {})
            if 0 in sides and 1 in sides and a.get(0, 0) >= 0.002 and a.get(1, 0) >= 0.002:
                rw.append(sides[0] / a[0])
                rc.append(sides[1] / a[1])
        if len(rw) > n0:
            w, c = sorted(rw[n0:]), sorted(rc[n0:])
            rows.append("%s: %d pairs, light wall %.4f cap %.4f" % (name, len(rw) - n0, w[len(w) // 2], c[len(c) // 2]))
    if not rows:
        return
    ratios = sorted(c / w for w, c in zip(rw, rc) if w > 0)
    med = ratios[len(ratios) // 2]
    mw, mc = sorted(rw)[len(rw) // 2], sorted(rc)[len(rc) // 2]
    add("light_albedo", "light across the mouth's edge with the textures taken out: the cap's light vs the wall's (within one stop)",
        verdict(0.5 <= med <= 2.0) if len(ratios) >= 20 else "BLOCKED",
        "%d pairs: median light wall %.4f, cap %.4f, median per-pair ratio %.3f; %s" % (len(ratios), mw, mc, med, " | ".join(rows)), {"pairs": len(ratios), "ratio": med})


def judge_seams(add, res, tag, names, sfx):
    light_rows, agg = [], {"pairs": 0, "wall": 0.0, "cap": 0.0, "dark": 0}
    for name in names:
        pr = load(tag, name)
        if not pr or "seam" not in pr:
            continue
        sm = pr["seam"]
        n = sm["n"]
        bad_miss = sm["missing"] > max(1, 0.003 * n)
        if sm["targets"] < 20 or n < 50:
            v = "BLOCKED"
        else:
            v = verdict(sm["void"] == 0 and not bad_miss)
        add(name, "the cut's seams (mouth edges, tunnel walls within one cell of the host face): no void, no missing surface", v,
            "%d seam faces, %d cap points in view; %d samples near the cut: ok %d, VOID %d, MISSING %d, near_other %d; aimed at cap points %d (void/missing %d); eye dug %s" %
            (sm["faces"], sm["targets"], n, sm["ok"], sm["void"], sm["missing"], sm["near_other"], sm["aimed"], sm["aimed_bad"], pr.get("camera", {}).get("dug")), {"seam": sm})
        if "edge" in pr:
            res[-1]["msg"] += "; %d of the ok samples are on a silhouette edge (aliasing, re-traced 1.5 px aside)" % pr["edge"]
        lt = pr.get("light")
        if lt:
            light_rows.append("%s: pairs %d/%d wall %.4f cap %.4f ratio %.3f (median %.3f) dark %d; tunnel %.4f (%d) host %.4f (%d)" %
                              (name, lt["pairs"], lt["aimed"], lt["lum_wall"], lt["lum_cap"], lt["ratio_mean"], lt["ratio_median"], lt["dark_pairs"], lt["tunnel_lum"], lt["tunnel_n"], lt["host_lum"], lt["host_n"]))
            agg["pairs"] += lt["pairs"]
            agg["wall"] += lt["lum_wall"] * lt["pairs"]
            agg["cap"] += lt["lum_cap"] * lt["pairs"]
            agg["dark"] += lt["dark_pairs"]
            agg.setdefault("ratios", []).extend([lt["ratio_median"]] * lt["pairs"] if lt["pairs"] else [])
            agg["sat"] = agg.get("sat", 0) + lt.get("saturated", 0)
            agg["toodark"] = agg.get("toodark", 0) + lt.get("too_dark", 0)
    # (bridge #79) light across the mouth's edge: Portal 2's wall face against the cap / block beside it, over every probe that saw pairs.
    # Continuous = the cap's mean luminance within one stop of the wall's (0.5 .. 2) and at most 10 % of the pairs darker than a quarter.
    if agg["pairs"]:
        r = agg["cap"] / agg["wall"] if agg["wall"] > 0 else -1
        rs = sorted(agg.get("ratios", []))
        med = rs[len(rs) // 2] if rs else -1
        v = verdict(0.5 <= r <= 2.0 and 0.5 <= med <= 2.0 and agg["dark"] <= 0.1 * agg["pairs"]) if agg["pairs"] >= 20 else "BLOCKED"
        add("light" + sfx, "luminance across the mouth's edge: the wall face vs the cap / first block beside it (within one stop)", v,
            "%d pairs judged (%d saturated walls and %d near-black walls left out): wall %.4f, cap %.4f, ratio of means %.3f, pair-weighted median of the probes' median ratios %.3f, %d pairs darker than a quarter of their wall; per probe: %s" %
            (agg["pairs"], agg.get("sat", 0), agg.get("toodark", 0), agg["wall"] / agg["pairs"], agg["cap"] / agg["pairs"], r, med, agg["dark"], " | ".join(light_rows)),
            {"pairs": agg["pairs"], "ratio": r, "dark": agg["dark"]})


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["run", "judge", "script"])
    ap.add_argument("tag")
    ap.add_argument("--so", default="")
    ap.add_argument("--map", default="pc_testbed")
    ap.add_argument("--mc", default="mirror", choices=["mirror", "terrain"])
    ap.add_argument("--mode", default="mc", choices=["mc", "chell"])
    ap.add_argument("--pre", default="", help="console commands before the map loads, ';'-separated (e.g. 'pc_dig_render 2')")
    ap.add_argument("--scen", default="s30,s35,s32,s33,s31")
    a = ap.parse_args()
    if a.cmd == "judge":
        judge(a.tag)
        return
    scen = [s for s in a.scen.split(",") if s]
    d = os.path.join(OUT, a.tag)
    os.makedirs(d, exist_ok=True)
    lines = build(a.tag, a.map, scen, a.mode, a.pre)
    sp = os.path.join(d, "script.txt")
    open(sp, "w").write("\n".join(lines) + "\n")
    json.dump({"tag": a.tag, "so": a.so, "map": a.map, "pre": a.pre, "scen": scen, "mode": a.mode, "time": time.strftime("%Y-%m-%d %H:%M:%S")}, open(os.path.join(d, "manifest.json"), "w"), indent=1)
    if a.cmd == "script":
        print(sp)
        return
    if not a.so or not os.path.isabs(a.so):
        sys.exit("--so must be an absolute path")
    if subprocess.call(["pgrep", "-x", "portal2_linux"], stdout=subprocess.DEVNULL) == 0:
        sys.exit("a Portal 2 is running (the user?): not launching")
    est = 240 + sum(int(l.split()[1]) / 60 for l in lines if l.startswith("wait "))
    env = dict(os.environ)
    env["PCB_SO"] = a.so
    pre = [c for c in a.pre.split(";") if c.strip()]
    if a.mode == "mc":
        pre.append("pc_start_mode mc")
    if pre:
        env["PCB_PRE"] = ";".join(pre)
    print("[dh] about %d s of script; %s" % (est, d))
    r = subprocess.call([os.path.join(ROOT, "bridge", "tools", "locked_run.sh"), a.mc, sp, a.map, str(int(est * 1.5) + 180), "dh_" + a.tag], env=env, cwd=os.path.join(ROOT, "bridge"))
    print("[dh] game run exited %d" % r)
    for name in ("bridge.log",):
        src = os.path.join(ROOT, "cache", "bridge", name)
        if os.path.exists(src):
            subprocess.call(["cp", "-f", src, os.path.join(d, name)])
    mcl = os.path.join(ROOT, "cache", "bridge", "shots", "dh_" + a.tag, "mc.log")
    if os.path.exists(mcl):
        subprocess.call(["cp", "-f", mcl, os.path.join(d, "mc.log")])
    judge(a.tag)


if __name__ == "__main__":
    main()
