#!/usr/bin/env python3
"""Bridge #26: the per-mover pass/fail table of a movers sweep run (tools/movers_sweep.py), from its bridge.log.

    tools/movers_table.py cache/bridge/shots/<run>/bridge.log

stand: Minecraft stood on a mover after pcb_stand_on (pass), or not (FAIL: no top found, or no mover under the feet);
ride: Minecraft was carried by a moving mover (ticks) when the map's inputs moved it, with no fall guard catch;
"n/a" where the mover has no input in the map (standing only).
"""
import re
import sys

rows, cur = [], None
for line in open(sys.argv[1], errors="replace"):
    m = re.search(r"\[bridge\] log: MV (\d+) (\S+) (\S+) (\S+) inputs (\d+)", line)
    if m:
        cur = {"n": int(m.group(1)), "cls": m.group(2), "name": m.group(3), "spec": m.group(4), "inputs": int(m.group(5)), "stand": "-", "ride": "n/a",
               "note": ""}
        rows.append(cur)
        continue
    if cur is None:
        continue
    if re.search(r"\[bridge\] log: MV \d+ end", line):
        cur = None
        continue
    if "invisible blocker" in line:
        cur["stand"] = "n/a"
        cur["note"] = "excluded: invisible blocker (only its floor over nothing is collision)"
        cur["inputs"] = 0
    elif "did not move" in line and cur["stand"] == "pass":
        cur["ride"] = "n/a (its inputs did not move it)"
    elif "not standable" in line:
        cur["stand"] = "n/a"
        cur["note"] = "no top of its own to stand on (" + line.split(" on ", 2)[-1].strip()[:50] + ")"
        cur["inputs"] = 0
    elif "ASSERT" in line and "stand on" in line:
        cur["stand"] = "FAIL"
        cur["note"] = "no top: " + line.split("stand on", 1)[1].strip()[:80]
    elif "ASSERT" in line and "stands on" in line:
        ok = "ASSERT pass" in line
        if cur["stand"] not in ("FAIL", "n/a"):
            cur["stand"] = "pass" if ok else "FAIL"
        r = re.search(r"riding (.+?)(, target| \(flags)", line)
        cur["on"] = r.group(1) if r else ""
    elif "ASSERT" in line and "rode the" in line and cur["stand"] != "n/a":
        t = re.search(r"(\d+) ticks carried", line)
        c = re.search(r"(\d+) fall guard catches", line)
        cur["ride"] = ("pass" if "ASSERT pass" in line else "FAIL") + " (%s ticks, %s catches)" % (t.group(1) if t else "?", c.group(1) if c else "?")

print("| # | mover | class | stand | rides when moved | on (Minecraft's ride) |")
print("|---|---|---|---|---|---|")
for r in rows:
    print("| %d | %s (%s) | %s | %s | %s | %s |" % (r["n"], r["name"], r["spec"], r["cls"], r["stand"], r["ride"] if r["inputs"] else "n/a (no input)", r.get("on", r["note"])))
n = len(rows)
st = sum(r["stand"] == "pass" for r in rows)
rd = [r for r in rows if r["inputs"]]
print("\n%d movers; stood on %d; with inputs %d, rode %d" % (n, st, len(rd), sum(r["ride"].startswith("pass") for r in rd)))
