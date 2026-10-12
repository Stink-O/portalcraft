#!/usr/bin/env python3
"""bridge #47: a test script for the portal onto Minecraft (G) in a campaign chamber (the mirror world).

  tools/gen_stage47.py <map> <x> <y> <z> [yaw] > tools/cfg/stage47_<map>.txt
  PCB_SO=<abs .so> tools/locked_run.sh mirror tools/cfg/stage47_<map>.txt <map> 420 <run>

Chell stands at x y z (a spot in the chamber), turns to the nearest white wall, holds G while firing a portal at it (the stage turns on, the
cliff exit is found, the partner opens there), looks at the portal, walks through into the Minecraft stage, looks around, comes back through
the cliff portal. Screenshots go to cache/bridge/shots/<run>/.
"""
import sys

m, x, y, z = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
yaw = sys.argv[5] if len(sys.argv) > 5 else "0"
tag = "s47_" + m.replace("sp_a", "a").replace("_", "")[:14]
print(f"""# stage47 (bridge #47) {m}: G in a campaign chamber -> the Minecraft stage, through the portal, and back
pcb_assert_soft 1
sv_cheats 1
waituntil spawned 600
waituntil link 9000
bridge_mode chell
waituntil chellmode 900
wait 90
pcb_noclip_set 0
setpos {x} {y} {z}
setang 0 {yaw} 0
wait 60
pcb_log S1 aim at a white wall
pcb_aim_portalable
wait 10
pcb_assert_stage 0
pcb_log S2 G held: arm, the stage starts
pcb_key_hold arm 3
wait 20
pcb_assert_stage 1
pcb_stage
wait 240
pcb_stage
pcb_log S3 fire the portal with G still held
pcb_key_hold arm 2
wait 6
+attack
wait 8
-attack
wait 360
pcb_mc_portal_state
pcb_assert_portals
pcb_assert_mc_anchor 0
pcb_assert_redirect
pcb_log S4 look at the portal from close
pcb_goto_portal 0 140
wait 5
pcb_aim_track 0 0.2 0
wait 120
screenshot pcb_{tag}_view
wait 30
screenshot pcb_{tag}_view2
pcb_aim_track off
pcb_log S5 walk through
pcb_assert_teleports reset
pcb_launch_to p0 175
wait 150
pcb_assert_teleports 1
pcb_assert_player_region 1
pcb_where
screenshot pcb_{tag}_in
wait 30
setang 10 180 0
wait 20
screenshot pcb_{tag}_in2
setang 10 0 0
wait 20
screenshot pcb_{tag}_in3
pcb_log S6 back through the cliff portal
pcb_goto_portal 1 120
wait 5
pcb_stop
wait 10
pcb_aim_track 1 0.2 0
wait 90
screenshot pcb_{tag}_back_view
pcb_assert_teleports reset
pcb_launch_to p1 175
wait 150
pcb_assert_teleports 1
pcb_assert_player_region 0
pcb_where
screenshot pcb_{tag}_back
wait 30
pcb_log stage47 done
quit""")
