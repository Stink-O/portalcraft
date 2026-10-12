#!/usr/bin/env python3
"""Bridge #31: tools/cfg/tb_mechanics.txt, generated from the testbed layout JSON: the Minecraft player USES each mechanic (walks, jumps,
drops, stands in it) and the test asserts the measured effect from logged state (pcb_mcm / pcb_assert_mcm motion metrics, pcb_mc_hurt,
output counters, entity counts, transit asserts). Screenshots are supporting evidence only.

    tools/gen_tb_mechanics.py [--only faith,funnel,...]
    PCB_SO=<abs .so> tools/locked_run.sh mirror tools/cfg/tb_mechanics.txt pc_testbed 2400 tb_mech

Positions come from the layout JSON (bay spots, entity origins, boxes), never typed by hand; facings are Source yaws (0 +x, 90 +y, 180 -x, 270 -y).
"""
import json
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
L = json.load(open(os.path.join(HERE, "cfg", "pc_testbed_layout.json")))
B = L["bays"]
FPS = 60    # `wait N` is N/60 s of wall clock in the script runner


def fr(s):
    return int(s * FPS)


def ent(bay, name):
    return B[bay]["entities"][name]


def yaw_of(dx, dy):
    return round(math.degrees(math.atan2(dy, dx)) % 360)


def head(name, text):
    """A new test block: asserts run again, the player is alive at full health with a strong hidden Regeneration (the test reset gives it; damage events are still logged)."""
    return ["pcb_test_block %s" % name, "pcb_log TB MECH ---- %s ----" % text, "pcb_mc_reset_player", "wait 60", "waituntil mcalive 600", "wait 20"]


def place(x, y, z, yaw, fall=8):
    """The Minecraft player at a Portal 2 point; verified: if Minecraft is not on the ground there within 8 u the block FAILS (placement failed) and its other asserts are NOT RUN."""
    return ["pcb_mc_place %.1f %.1f %.1f %d %d" % (x, y, z, yaw, fall), "waituntil mcplaced 900", "wait 30", "pcb_setview 0 %d" % yaw, "wait 6", "pcb_assert_placed"]


def walk(seconds, key=26):
    return ["pcb_mckey %d 1" % key, "wait %d" % fr(seconds), "pcb_mckey %d 0" % key]


def relay(bay, which="go"):
    return "ent_fire %s trigger" % B[bay]["relays"][which]["name"]


def field(cls, name, fld, val):
    return "pcb_assert_ent_field %s %s %s %d" % (cls, name, fld, val)


def tests():
    t = []
    # ---------------------------------------------------------------- faith plate
    f = ent("faith", "tb_faith_arm")["origin"]
    tgt = ent("faith", "tb_faith_target")["origin"]
    fy = yaw_of(tgt[0] - f[0], tgt[1] - f[1])
    t.append(("faith", head("faith", "faith plate: step on it, get launched, land at tb_faith_target") + [relay("faith"), "wait 90", field("trigger_catapult", "tb_faith", "m_bDisabled", 0)] +
              place(f[0] - 90 * math.cos(math.radians(fy)), f[1] - 90 * math.sin(math.radians(fy)), 1, fy) + ["pcb_mcm reset"] + walk(0.8) + ["wait %d" % fr(5),
              "pcb_assert_mcm rise 48", "pcb_assert_mcm spd_v 250", "pcb_assert_mc_near tb_faith_target 200 64", relay("faith", "reset"), "wait 60"]))
    # ---------------------------------------------------------------- excursion funnel
    fu = ent("funnel", "tb_funnel")["origin"]
    ux = B["funnel"]["into_dir"]
    sx, sy = fu[0] - ux[0] * 90, fu[1] - ux[1] * 90   # 90 u from the axis, on the hall side
    t.append(("funnel", head("funnel", "excursion funnel: walk into the beam, be carried up its axis") + [relay("funnel"), "wait 90", field("prop_tractor_beam", "tb_funnel", "m_bEnabled", 1)] +
              place(sx, sy, 1, yaw_of(fu[0] - sx, fu[1] - sy)) + ["pcb_mcm reset"] + walk(0.7) + ["wait %d" % fr(1.2), "pcb_assert_mc_near tb_funnel 70", "pcb_assert_mcm rise 100", "pcb_assert_mcm spd_v 60",
              relay("funnel", "reset"), "wait 60"]))
    # ---------------------------------------------------------------- turret
    tu = ent("turret", "tb_turret")["origin"]
    t.append(("turret", head("turret", "turret: stand in its view; it fires, Minecraft takes damage from it; disabled, it stops") + [relay("turret"), "wait 90",
              field("npc_portal_turret_floor", "tb_turret", "m_bEnabled", 1)] + place(tu[0] - 220, tu[1], 1, 0) + ["pcb_mc_hurt mark", "wait %d" % fr(3), "pcb_glhud_shot mech_turret_fire", "wait %d" % fr(3),
              "pcb_mc_hurt atleast 2 generic near", relay("turret", "reset"), "wait 90", field("npc_portal_turret_floor", "tb_turret", "m_bEnabled", 0), "pcb_mc_reset_player", "wait 30",
              "pcb_mc_hurt mark", "wait %d" % fr(4), "pcb_mc_hurt assert 0 generic near"]))
    # ---------------------------------------------------------------- light bridge
    br = ent("bridge", "tb_bridge")["origin"]
    ud = B["bridge"]["u_dir"]
    by = yaw_of(ud[0], ud[1])
    t.append(("bridge", head("bridge", "light bridge: walk along it over the pit, feet stay on it; switched off, fall") + [relay("bridge", "reset"), "wait 60", relay("bridge"), "wait %d" % fr(4),
              field("prop_wall_projector", "tb_bridge", "m_bEnabled", 1)] + place(br[0], br[1] + ud[1] * 25, 60, by, 30) + ["pcb_glhud_shot mech_bridge_placed", "pcb_mcm reset"] + walk(1.0) +
              ["wait 20", "pcb_glhud_shot mech_bridge_walked", "pcb_assert_mcm drop 0 8", "pcb_assert_mcm dy -400 -100" if ud[1] < 0 else "pcb_assert_mcm dy 100 400", "pcb_assert_mc_flag ground 1",
               relay("bridge", "reset"), "wait %d" % fr(3), field("prop_wall_projector", "tb_bridge", "m_bEnabled", 0), "pcb_assert_mcm drop 100"]))
    # ---------------------------------------------------------------- fizzler: a cube, then the player
    fz = ent("fizzler", "tb_fizzler")
    fx, fyy = (fz["mins"][0] + fz["maxs"][0]) / 2, (fz["mins"][1] + fz["maxs"][1]) / 2
    t.append(("fizzler", head("fizzler", "fizzler: a cube in the grid dissolves; the player walking through clears portals") + [field("trigger_portal_cleanser", "tb_fizzler", "m_bDisabled", 0),
              "pcb_spawn_cube %.0f %.0f 40 tb_fz_cube" % (fx, fyy), "pcb_spawn_cube %.0f %.0f 40 tb_fz_ctl" % (fx - 120, fyy), "wait %d" % fr(2),
              "pcb_assert_ent_count tb_fz_cube 0", "pcb_assert_ent_count tb_fz_ctl 1", relay("portals"), "wait 60", field("prop_portal", "tb_pair_a", "m_bActivated", 1)] +
              place(fx - 110, fyy, 1, 0) + walk(1.4) + ["wait 60", field("prop_portal", "tb_pair_a", "m_bActivated", 0), field("prop_portal", "tb_pair_b", "m_bActivated", 0)]))
    # ---------------------------------------------------------------- laser
    em = ent("laser", "tb_laser_emitter")["origin"]
    ca = ent("laser", "tb_laser_catcher")["origin"]
    mid = ((em[0] + ca[0]) / 2, (em[1] + ca[1]) / 2)
    ly = yaw_of(ca[0] - em[0], ca[1] - em[1])
    side = (mid[0] + 140 * math.cos(math.radians(ly + 90)), mid[1] + 140 * math.sin(math.radians(ly + 90)))
    lines = head("laser", "laser: switched on, the catcher fires, the beam hurts, a reflective cube in the beam powers the relay") + [relay("laser"), "wait %d" % fr(3), "pcb_assert_ent_float math_counter tb_cnt_catcher m_OutValue 1 99"]
    lines += place(mid[0], mid[1], 1, ly) + ["pcb_mc_hurt mark", "wait %d" % fr(3), "pcb_mc_hurt atleast 1 generic near", "pcb_mc_reset_player", "wait 30"] + place(side[0], side[1], 1, ly)
    for k, cy in enumerate((0, 90, 180, 270)):   # the reflective cube's turn that sends the beam at the relay: all four tried (Valve's output side is not known here)
        lines += ["pcb_spawn_ent prop_weighted_cube %.0f %.0f 64 %d tb_lc%d CubeType 1 NewSkins 1" % (mid[0], mid[1], cy, k), "wait 3", "ent_fire tb_lc%d DisableMotion" % k, "wait %d" % fr(1.5),
                  "ent_fire tb_lc%d Kill" % k, "wait 20"]
    lines += ["pcb_assert_ent_float math_counter tb_cnt_relay m_OutValue 1 99"]
    t.append(("laser", lines))
    # ---------------------------------------------------------------- buttons
    fb = ent("cubes", "tb_btn_floor")["origin"]
    pb = ent("cubes", "tb_btn_pedestal")["origin"]
    t.append(("buttons", head("buttons", "buttons: a cube on the floor button presses it; the player uses the pedestal button") + ["pcb_assert_ent_float math_counter tb_cnt_floor m_OutValue 0",
              "pcb_spawn_cube %.0f %.0f 90 tb_btn_cube" % (fb[0], fb[1]), "wait %d" % fr(3), "pcb_assert_ent_float math_counter tb_cnt_floor m_OutValue 1 5"] +
              place(pb[0], pb[1] + 70, 1, 270) + ["pcb_setview 20 270", "wait 20", "pcb_mckey rmb 1", "wait 12", "pcb_mckey rmb 0", "wait %d" % fr(2),
              "pcb_assert_ent_float math_counter tb_cnt_pedestal m_OutValue 1 5"]))
    # ---------------------------------------------------------------- portals (walk through the pair)
    a = ent("portals", "tb_pair_a")
    an = a["normal"]
    t.append(("portals", head("portals", "portals: walk into tb_pair_a, come out of tb_pair_b with the speed kept") + [relay("portals"), "wait 60", field("prop_portal", "tb_pair_a", "m_bActivated", 1),
              "pcb_assert_mc_transits reset"] + place(a["origin"][0] + an[0] * 150, a["origin"][1] + an[1] * 150, 1, yaw_of(-an[0], -an[1])) + ["pcb_mcm reset"] + walk(1.2) +
              ["wait 60", "pcb_assert_mc_transits 1", "pcb_assert_mc_transit_speed 12", "pcb_assert_mc_near tb_pair_b 260 120", "pcb_assert_mcm spd_h 140"]))
    # ---------------------------------------------------------------- fling
    fl = ent("fling", "tb_fling_floor_portal")["origin"]
    hi = ent("fling", "tb_spot_fling_high")["origin"]
    fy = yaw_of(fl[0] - hi[0], fl[1] - hi[1])
    t.append(("fling", head("fling", "fling: walk off the 640 u ledge onto the floor portal, leave the wall portal fast") + [relay("fling"), "wait 60", field("prop_portal", "tb_fling_floor_portal", "m_bActivated", 1),
              "pcb_assert_mc_transits reset"] + place(hi[0], hi[1], hi[2], fy, 8) + ["pcb_mcm reset"] + walk(3.0) + ["wait %d" % fr(3), "pcb_assert_mc_transits 1", "pcb_assert_mc_transit_speed 12",
              "pcb_assert_mcm spd_h 400", relay("fling", "reset"), "wait 60"]))
    # ---------------------------------------------------------------- doors
    for bay, nm, cls in (("doors", "tb_door", "prop_testchamber_door"), ("doorh", "tb_door_h", "func_door")):
        e = ent(bay, nm)
        c = e["origin"] if "origin" in e else ((e["mins"][0] + e["maxs"][0]) / 2, (e["mins"][1] + e["maxs"][1]) / 2)
        ix = B[bay]["into_dir"]
        iy = yaw_of(ix[0], ix[1])
        dp = (c[0] - 70 * ix[0], c[1] - 70 * ix[1])
        t.append((bay, head(bay, "%s: closed it blocks the player, opened the player passes" % nm) + place(dp[0], dp[1], 1, iy) + ["pcb_mcm reset"] + walk(1.0) + ["wait 20", "pcb_assert_mcm dist_h 0 90",
                  relay(bay), "wait %d" % fr(4)] + ([field(cls, nm, "m_bIsOpen", 1)] if cls == "prop_testchamber_door" else []) + place(dp[0], dp[1], 1, iy) + ["pcb_mcm reset"] + walk(1.0) +
                  ["wait 20", "pcb_assert_mcm dist_h 130", relay(bay, "reset"), "wait %d" % fr(4)]))
    # ---------------------------------------------------------------- movers
    from gen_tb_smoke import bay_lines
    for n in ("lift", "mlinear", "train"):
        t.append((n, head(n, "mover %s: ride it" % n) + bay_lines(L, n, True)))
    # the turntable, the tilting panel, the door panel: placed on their tops (verified), then moved by the map; off-pivot on the turntable
    for n, nm, at in (("rotate", "tb_turntable", B["rotate"].get("ride_at")), ("tilt", "tb_tilt", None), ("doorpanel", "tb_doorpanel_lift", None)):
        e = ent(n, nm)
        ctr = at if at else [e["center"][0], e["center"][1], e["top"]]
        t.append((n, head(n, "mover %s: stand on it, let the map move it, ride (turntable: off the pivot, the view turns with it)" % n) + place(ctr[0], ctr[1], ctr[2] + 1, B[n]["spot"]["yaw"], 8) +
                  ["pcb_assert_mc_platform 1 %s" % nm, "pcb_assert_mc_rode reset %s" % nm, "pcb_ride_spin reset", relay(n), "wait %d" % fr(B[n]["settle_s"]), "pcb_assert_mc_rode 8 0 %s" % nm] +
                  (["pcb_ride_spin assert 30 12"] if n == "rotate" else []) + [relay(n, "reset"), "wait %d" % fr(B[n]["settle_s"])]))
    # ---------------------------------------------------------------- goo (last: it kills)
    gh = ent("goo", "tb_goo_hurt")
    gx, gy = (gh["mins"][0] + gh["maxs"][0]) / 2, (gh["mins"][1] + gh["maxs"][1]) / 2
    ix = B["goo"]["into_dir"]
    t.append(("goo", head("goo", "goo: walk off the pit edge into it, die") + [field("trigger_hurt", "tb_goo_hurt", "m_bDisabled", 0)] + place(gh["mins"][0] - 60, gy, 1, yaw_of(ix[0], ix[1])) +
              walk(2.5) + ["wait %d" % fr(2), "pcb_assert_mc_flag dead 1", "pcb_mc_reset_player"]))
    return t


def main():
    only = sys.argv[sys.argv.index("--only") + 1].split(",") if "--only" in sys.argv else None
    lines = ["# tb_mechanics (generated by tools/gen_tb_mechanics.py from pc_testbed_layout.json; do not edit by hand)",
             "# the Minecraft player uses each mechanic; the asserts read measured effects (pcb_mcm, hurt events, output counters, entity counts, transits)",
             "sv_cheats 1", "pcb_assert_soft 1", "waituntil spawned 600", "waituntil link 9000", "wait 240",              "noclip", "pcb_goto tb_spot_portals 64", "wait 100", "noclip", "bridge_mode mc", "waituntil mcmode 2400", "waituntil mcleads 2400", "wait 120",
             "pcb_mc_cmd attribute @p minecraft:max_health base set 20", "pcb_mc_reset_player", "wait 60", "pcb_assert_mc_feet_above reset", "pcb_mc_hurt mark"]
    for n, ls in tests():
        if only and n not in only:
            continue
        lines += ls
    lines += ["pcb_mc_cmd effect clear @p", "pcb_mc_cmd attribute @p minecraft:max_health base set 20", "pcb_movers write", "pcb_log TB MECH done", "pcb_run_summary", "quit"]
    open(os.path.join(HERE, "cfg", "tb_mechanics.txt"), "w").write("\n".join(lines) + "\n")
    print("wrote tb_mechanics.txt", len(lines), "lines")


if __name__ == "__main__":
    sys.path.insert(0, HERE)
    main()
