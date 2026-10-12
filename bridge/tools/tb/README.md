# pc_testbed suite (lead, 2026-10-03)

This suite replaces `gen_tb_smoke.py` / `gen_tb_mechanics.py`. Each test:
1. puts the player in a known state (bay reset, a verified placement);
2. uses the mechanic with the player's own inputs (walk, jump, use, shoot);
3. **records** what happened.

Nothing is asserted in the game. `judge.py` reads each recording afterwards and gives one of these verdicts:

| verdict | meaning |
|---|---|
| PASS | every expectation held |
| FAIL | the setup was right and the feature misbehaved: a real gap |
| BLOCKED | the setup didn't happen (placement missed, the mechanic itself never moved, the cube wasn't there). This says nothing about the feature; fix the test |
| NO TRACE | the run never got to this test |

A failure never stops the run. A verdict can be re-judged without the game: `tools/tbtest judge <tag>`.

```
tools/tbtest list [-k lift,faith]
tools/tbtest run <tag> --so /abs/plugin.so [-k pat,...] [--modes chell|mc]
tools/tbtest run <tag> -k arrival.walk_out.chell                   # start-of-map tests have their own run
tools/tbtest run <tag> -k arrival.walk_out.mc --start mc
tools/tbtest judge <tag>
```

Output goes to `cache/bridge/tb/<tag>/`:
- `report.html`: a bay x mode grid, then one card per test with its checks and measured numbers, a height/distance-over-time plot, and screenshots;
- `results.json`, the traces (`<test>.jsonl`), `script.txt`, `bridge.log` and `mc.log`.

## Pieces
- `src/bridge/tbtrace.cpp` (plugin):
  - `pcb_tb_run <dir>`, `pcb_tb_begin <test>`, `pcb_tb_end`;
  - `pcb_tb_event <label>`;
  - `pcb_tb_watch <label> <class|*> <name|*> <field> <i|f|b|o|a|n|c>`.

  Every game frame (about 30-60 Hz) it writes:
  - Portal 2's player: feet, velocity, view, health, holding;
  - Minecraft's player: feet, ground flag, flags, ride mover, transit count;
  - every prop_portal;
  - the watched fields.

  At the end it appends Minecraft's damage events since begin, from mc.log.
- `core.py`: the registry (`@test(bay, modes, title, needs, order, run)`) and the script builder `T`:
  - `place` (verified), `drop_in`, `hold`/`tap` (one name, mapped to Chell's buttons or Minecraft's SDL scancodes);
  - `face`, `look`, `relay`, `fire`, `watch*`, `event`, `shot`.
- `tests.py`: the tests. Positions come from `cfg/pc_testbed_layout.json`, never typed in.
- `judge.py`: `R` gives checks:
  - `require` (else BLOCKED) and `expect` (else FAIL);
  - `pos`/`path`/`rise`/`drop`/`speed_h`/`jumps` (transits);
  - `damage` (Chell: health lost; Minecraft: hurt events with source);
  - `w` (watched values) and `portal_near`.

## Rules for new tests
- Check the setup with `require`: placement (automatic), "the mechanic itself moved", "the cube is there".
- Check the effect **and its cause**. For example: carried up *as far as the mover rose*; hurt *while in the beam*; portals closed *after crossing the grid*.
- Use the mechanic. Don't teleport to the result.
- Put the measured numbers in every message, pass or fail.
- Tests that can kill Chell (turret, laser, goo) get `order >= 85`. Chell's death reloads the map, so they run last.
