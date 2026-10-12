#!/usr/bin/env python3
"""Writes tools/cfg/float4.txt: the floating-origin gate (bridge #10).

Far points are reached by teleport steps (pcb_float_step_to: a box-safe teleport towards a Minecraft point, so
every step goes through the shift logic), with asserts after every step: Chell inside the region box, every
frame of the step clean (no white / region background), the corridor pair and the cube intact at their
Minecraft positions (open, or parked out of the box). Frame continuity at a shift is checked by short
continuous flights (~40 blocks) across one shift boundary per axis direction. The pair-persistence check (go
1500+ blocks away, come back, the pair still works) uses the steps too.

  tools/gen_float.py            -> tools/cfg/float4.txt
"""
import os

PAIR = (-376, 442)   # the test corridor (MC x, z)
Y = 168              # flight height (MC y)
FAR = 1520           # blocks out on each horizontal axis
STEPS = 5            # teleport steps per 1520-block leg (each <= ~400 blocks)

L = []
a = L.append


def header():
    a("# float4 (bridge #10, written by tools/gen_float.py): the floating-origin gate. Far points by teleport steps")
    a("# (each through the shift logic, asserted: inside the box, clean frames, pair + cube intact); frame continuity")
    a("# at a shift by short continuous flights across one boundary per axis direction; the pair still works after")
    a("# 1500+ blocks away and back.")
    a("pcb_assert_soft 1")
    a("sv_cheats 1")
    a("waituntil spawned 600")
    a("waituntil link 9000")
    a("waituntil haveexit 3600")
    a("pc_pit_redirect 0")
    a("pcb_chell_proxy 0")
    a("pcb_mc_cmd time set noon")
    a("pc_mc_float 0")


def setup():
    a("pcb_log FLOAT setup: a pair on the corridor walls and a cube on its floor beside them")
    a("pcb_noclip_set 1")
    a("setpos -1911 -194 -15100")
    a("pcb_stop")
    a("wait 30")
    a("waituntil near 1800")
    a("waituntil terrain_idle 1200")
    a("pcb_build_corridor -376 89 442 12")
    a("wait 30")
    a("waituntil corridor 1200")
    a("waituntil terrain_idle 1200")
    for i, b in ((0, "attack"), (1, "attack2")):
        a(f"pcb_corridor_aim {i}")
        a("wait 20")
        a(f"+{b}")
        a("wait 6")
        a(f"-{b}")
        a("wait 40" if i == 0 else "wait 60")
    a("pcb_assert_portals")
    a("pcb_assert_portal_face 0")
    a("pcb_assert_portal_face 1")
    a("pcb_spawn_cube -1760 -820 -14138 pcb_cube")
    a("wait 300")
    a("pcb_assert_prop_rest pcb_cube 89")
    a("pcb_float_mark")


def command_shifts():
    for n, (dist, dz) in enumerate(((10, -3), (6, 3)), 1):
        a(f"pcb_log FLOAT shift {n} (test command, 3 chunks {'north' if dz < 0 else 'south'}) looking at the pair")
        a(f"pcb_view_from 1 {dist}")
        a("wait 300")
        if n == 1:
            a("pcb_assert_light_smooth reset")
        warm()
        a(f"pcb_frame_watch 6000 shift{n} mcworld")
        a("pcb_twitch reset")
        a("wait 30")
        a(f"pcb_float_shift 0 {dz}")
        a("wait 120")
        a(f"pcb_twitch report shift{n}_still 1.0 4")
        watch_end(f"shift{n}", 5)
        a("pcb_assert_float_kept 0.3")
        a("pcb_assert_light_smooth 0.25")


def warm():
    # (a short watch first: the first frames of a watch have shown 7-11 frame old images even before anything
    # happened, run float4a f1-f3; measured watches start after it)
    a("pcb_frame_watch 30 warm mcworld")
    a("wait 40")
    a("pcb_frame_watch 0")


def watch_end(label, shift_frames=0):
    a("pcb_frame_watch 0")
    a("wait 5")
    a("pcb_assert_watch")
    if shift_frames:
        a(f"pcb_assert_watch_shifts {shift_frames}")


def launch(label):
    a(f"pcb_log FLOAT {label}: launched into portal 1")
    a("pcb_goto_portal 1 120")
    a("pcb_stop")
    a("pcb_noclip_set 0")
    a("wait 10")
    a("pcb_assert_teleports reset")
    a("pcb_launch_to p1 350")
    a("wait 150")
    a("pcb_assert_teleports 1")
    a("pcb_noclip_set 1")


def flights():
    a("pcb_log FLOAT continuous flights: ~40 blocks across one shift boundary per axis direction, every frame watched")
    a("pc_mc_float 1 200")
    a("sv_noclipspeed 5")
    a("setpos -1920 -700 -11000")
    a("pcb_stop")
    a("wait 120")
    steer = {"+x": "rel 60 0 0", "-x": "rel -60 0 0", "+z": "rel 0 0 60", "-z": "rel 0 0 -60",
             "up": "angles -89 0", "down": "angles 89 0"}
    for ax in ("+x", "-x", "+z", "-z", "up", "down"):
        a(f"pcb_log FLOAT flight {ax}")
        a(f"pcb_float_near_edge {ax} 20")
        a(f"pcb_steer {steer[ax]}")
        a("wait 60")
        a("waituntil streamidle 900")
        warm()
        a(f"pcb_frame_watch 6000 fly{ax} mcworld")
        a("+forward")
        a("waituntil floatshift 600")
        a("wait 60")
        a("-forward")
        a("pcb_steer off")
        a("pcb_stop")
        watch_end(f"fly{ax}", 5)
        a("pcb_assert_float_inbox")
        a("pcb_assert_float_intact")
    a("setang 0 0 0")


def step(label, x, y, z):
    a(f"pcb_frame_watch 2000 {label} mcworld")
    a(f"pcb_float_step_to {x} {y} {z}")
    a("wait 150")
    a("pcb_frame_watch 0")
    a("wait 5")
    a("pcb_assert_watch_nowhite")
    a("pcb_assert_float_inbox")
    a("pcb_assert_float_intact")


def leg(name, x, z, n=STEPS):
    a(f"pcb_log FLOAT steps {name}: towards MC ({x} {Y} {z})")
    a("pcb_float_trip start")
    a("wait 2")
    for i in range(1, n + 1):
        step(f"{name}{i}", x, Y, z)
    a(f"pcb_float_trip assert {FAR - 20}")


def far_points():
    px, pz = PAIR
    for _ in range(2):
        a(f"pcb_float_step_to {px} {Y} {pz}")
        a("wait 150")
    for name, dx, dz in (("east", FAR, 0), ("west", -FAR, 0), ("south", 0, FAR), ("north", 0, -FAR)):
        leg(f"{name}_out", px + dx, pz + dz)
        leg(f"{name}_back", px, pz)
        if name == "east":
            a("pcb_assert_float_parked 2 2 1")
            a("pcb_assert_portals")
            launch("the pair after 1500 blocks east and back (teleport steps)")
            a(f"pcb_float_step_to {px} {Y} {pz}")
            a("wait 150")


def vertical():
    a("pcb_log FLOAT vertical steps: up past the world's top (y 330), down to y -50 (deep caves), back to y 168")
    for i in range(1, 4):
        step(f"up{i}", "~", 335, "~")
    a("waituntil mcyabove330 60")
    for i in range(1, 6):
        step(f"down{i}", "~", -55, "~")
    a("waituntil mcybelow-50 60")
    for i in range(1, 4):
        step(f"back{i}", "~", Y, "~")


def finish():
    px, pz = PAIR
    a(f"pcb_float_step_to {px} {Y} {pz}")
    a("wait 150")
    a("pcb_assert_float_parked 2 2 1")
    a("pcb_assert_float_intact")
    a("pcb_assert_portals")
    launch("the pair at the end")
    a("pcb_log FLOAT home: close the pair, Chell back to the chamber, the mapping returns")
    a("ent_fire prop_portal Fizzle")
    a("setpos 128 -384 -500")
    a("pcb_stop")
    a("wait 240")
    a("pc_mc_float")
    a("pcb_assert_exit")
    a("pcb_build_corridor remove")
    a("wait 30")
    a("pcb_run_summary")
    a("quit")


header()
setup()
command_shifts()
launch("through the pair after the command shifts")
flights()
far_points()
vertical()
finish()
out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "cfg", "float4.txt")
with open(out, "w") as f:
    f.write("\n".join(L) + "\n")
print(f"{out}: {len(L)} lines")
