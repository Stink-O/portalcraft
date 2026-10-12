#!/usr/bin/env python3
"""Bridge #26: a test sweep over every mover of a map, from the map itself (no hand-picked spots).

    tools/movers_sweep.py <map> [--max N] > tools/cfg/<map>_movers26.txt
    PCB_SO=<abs .so> tools/locked_run.sh mirror tools/cfg/<map>_movers26.txt <map> 1500 <run>
    tools/movers_table.py cache/bridge/shots/<run>/bridge.log

Reads the BSP's entity lump: every entity of a mover class (as movers.cpp lists them), its parent (parentname, or a
SetParent output aimed at it), and the map's own outputs that move it (Open/Close/Toggle, SetAnimation, train inputs...),
its parent's too. Per mover, as the Minecraft player: stand on it (pcb_stand_on: the top under its box's middle), check
Minecraft stands on a mover there, fire its motion inputs in the map's order, and check Minecraft was carried by a moving
mover without falling through. Movers with no input are checked standing only ("natural motion" shows as rode ticks).
Physics props and bone followers are runtime entities: the run ends with `pcb_movers write` (every mover, bound or not).
"""
import re
import struct
import sys

from portal2_dir import portal2_dir  # finds Portal 2 ($PORTAL2_DIR or Steam's libraries)
GAME = portal2_dir() + "/portal2/maps/"
CLASSES = ["func_door", "func_door_rotating", "func_movelinear", "func_tracktrain", "func_train", "func_tanktrain", "func_brush",
           "func_rotating", "func_platrot", "func_wall_toggle", "func_button", "func_rot_button", "func_physbox", "prop_testchamber_door",
           "prop_dynamic", "prop_floor_button", "prop_under_floor_button", "prop_button", "prop_under_button", "prop_physics",
           "prop_physics_override", "prop_weighted_cube", "prop_monster_box", "npc_portal_turret_floor"]
MOTION = {"Open", "Close", "Toggle", "SetAnimation", "SetAnimationNoReset", "StartForward", "StartBackward", "Resume", "MoveToPathNode",
          "SetPosition", "Start", "Reverse", "Press", "PressIn", "PressOut", "SetSpeed", "SetSpeedReal"}
ORDER = {"prop_weighted_cube": 0, "prop_physics": 0, "prop_physics_override": 0, "prop_monster_box": 0, "npc_portal_turret_floor": 0,
         "prop_floor_button": 1, "prop_under_floor_button": 1, "prop_button": 1, "prop_under_button": 1, "func_button": 1,
         "func_brush": 2, "func_wall_toggle": 2, "prop_dynamic": 3, "prop_testchamber_door": 4, "func_door": 5, "func_door_rotating": 5,
         "func_movelinear": 5, "func_rotating": 5, "func_platrot": 5, "func_tracktrain": 9, "func_train": 9, "func_tanktrain": 9}


def entities(path):
    d = open(path, "rb").read()
    a, b, _, _ = struct.unpack_from("<iiii", d, 8)
    text = d[a:a + b].decode("latin1")
    out = []
    for m in re.finditer(r"\{([^{}]*)\}", text):
        kv = re.findall(r'"([^"]*)"\s+"([^"]*)"', m.group(1))
        out.append(kv)
    return out


def clip_only_models(path):
    """brush models whose every brush has no solid contents (playerclip / monsterclip only)"""
    d = open(path, "rb").read()
    L = [struct.unpack_from("<iiii", d, 8 + 16 * i) for i in range(64)]
    def lump(i):
        a, b, _, _ = L[i]
        return d[a:a + b]
    models, nodes, leafs, leafbrushes, brushes = lump(14), lump(5), lump(10), lump(17), lump(18)
    def collect(n, out, depth=0):
        if depth > 200:
            return
        if n < 0:
            flb, nlb = struct.unpack_from("<HH", leafs, (-1 - n) * 32 + 24)
            for i in range(flb, flb + nlb):
                out.add(struct.unpack_from("<H", leafbrushes, i * 2)[0])
            return
        c0, c1 = struct.unpack_from("<ii", nodes, n * 32 + 4)
        collect(c0, out, depth + 1)
        collect(c1, out, depth + 1)
    out = set()
    for m in range(1, len(models) // 48):
        bs = set()
        collect(struct.unpack_from("<i", models, 48 * m + 36)[0], bs)
        conts = [struct.unpack_from("<i", brushes, b * 12 + 8)[0] for b in bs]
        if conts and all(c & 0x4007 == 0 and c & 0x8 == 0 for c in conts):  # no SOLID/WINDOW/AUX/GRATE/MOVEABLE
            out.add(m)
    return out


def main():
    mapname = sys.argv[1]
    mx = int(sys.argv[sys.argv.index("--max") + 1]) if "--max" in sys.argv else 1000
    only = sys.argv[sys.argv.index("--only") + 1].split(",") if "--only" in sys.argv else None
    ents = entities(GAME + mapname + ".bsp")
    clip_models = clip_only_models(GAME + mapname + ".bsp")
    names = {}
    inputs = {}   # target -> [(input, param)]
    parent = {}
    for kv in ents:
        d = dict(kv)
        if d.get("targetname"):
            names.setdefault(d["targetname"], []).append(d)
        if d.get("parentname"):
            parent[d.get("targetname", "")] = d["parentname"]
        for k, v in kv:
            if not k.startswith("On"):
                continue
            f = re.split("[\x1b,]", v)
            if len(f) < 3:
                continue
            tgt, inp, par = f[0], f[1], f[2]
            if inp == "SetParent" and par:
                parent.setdefault(tgt, par)
            if inp in MOTION and (tgt, inp, par) not in [(tgt, i, p) for i, p in inputs.get(tgt, [])]:
                inputs.setdefault(tgt, []).append((inp, par))
    movers = []
    for kv in ents:
        d = dict(kv)
        c = d.get("classname", "")
        if c not in CLASSES or (c == "prop_dynamic" and d.get("solid", "6") == "0"):
            continue
        # (#23's rule: a brush entity made only of clip is not geometry; its floor-over-nothing part is checked by startmc25's walk-out)
        if d.get("model", "").startswith("*") and int(d["model"][1:]) in clip_models:
            continue
        name = d.get("targetname", "")
        o = [float(x) for x in d.get("origin", "0 0 0").split()]
        spec = name if name and len(names.get(name, [])) == 1 else "@%.0f,%.0f,%.0f" % (o[0], o[1], o[2])
        # what moves it: its own inputs, else its parents'
        drive, who = [], name
        seen = set()
        while who and who not in seen:
            seen.add(who)
            if inputs.get(who):
                drive = [(who, i, p) for i, p in inputs[who]]
                break
            who = parent.get(who, "")
        # a train may already be at the end its map input sends it to: back along its path, then forward again
        if c in ("func_tracktrain", "func_train", "func_tanktrain") and name:
            drive = drive + [(name, "StartBackward", ""), (name, "StartForward", "")]
        movers.append((ORDER.get(c, 6), c, name, spec, drive))
    movers.sort(key=lambda m: (m[0], m[1], m[2]))
    if only:
        movers = [m for m in movers if m[2] in only or m[3] in only]
    movers = movers[:mx]
    print("# %s_movers26 (generated by tools/movers_sweep.py %s, bridge #26): every mover of the map, as the Minecraft player:" % (mapname, mapname))
    print("# stand on it, check Minecraft stands on a mover, fire the map's own inputs that move it, check it rode. Table: tools/movers_table.py")
    print("sv_cheats 1\npcb_assert_soft 1\nwaituntil spawned 600\nwaituntil link 9000\nbridge_mode mc\nwaituntil mcmode 2400\nwaituntil mcleads 1200\nwait 300")
    print("pcb_movers write\npcb_assert_mc_feet_above reset")
    for n, (_, c, name, spec, drive) in enumerate(movers):
        print("pcb_log MV %d %s %s %s inputs %d" % (n, c, name or "-", spec, len(drive)))
        print("pcb_stand_on %s" % spec)
        print("wait 20\nwaituntil mcleads 600\nwait 30")
        print("pcb_where\npcb_assert_mc_platform 1 %s\npcb_assert_mc_rode reset %s" % (spec, spec))
        print("pcb_setview 50 90\nwait 2\npcb_glhud_shot mv26_%d_on" % n)
        for who, inp, par in drive:
            print("ent_fire %s %s %s" % (who, inp, par) if par else "ent_fire %s %s" % (who, inp))
            print("wait 90")
        print("pcb_where")
        if drive:
            print("pcb_glhud_shot mv26_%d_moved" % n)
            print("pcb_assert_mc_rode 4 0")
        print("pcb_log MV %d end" % n)
    print("pcb_movers write\npc_mc_fall_guard\npcb_run_summary\nquit")


main()
