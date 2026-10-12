#!/usr/bin/env python3
"""Summarise an mcworld run's bridge.log: asserts per section, twitch, image age, frame times.
  tools/mcworld_report.py cache/bridge/shots/<run>/bridge.log
"""
import re, sys

log = open(sys.argv[1], errors="replace").read().splitlines()
section = "setup"
rows = []          # (section, pass?, text)
twitch = []        # (label, mean, p95, p99, max)
ages = []          # (section, line)
prof = []
for ln in log:
    ln = ln.replace("[bridge] ", "")
    m = re.match(r"log: (CASE \S+|PAIR \S+|PART\S*|TRANSIT .*|VIEW \S+|PROF .*)", ln)
    if m:
        tag = m.group(1)
        if tag.startswith("PROF"):
            prof.append([tag, None])
        elif not tag.startswith(("TRANSIT", "VIEW")):
            section = tag.split(":")[0]
        continue
    if ln.startswith("host frames:") and prof and prof[-1][1] is None:
        prof[-1][1] = ln
    m = re.match(r"ASSERT (pass|FAIL): (.*)", ln)
    if m:
        rows.append((section, m.group(1) == "pass", m.group(2)))
        if "image age bounded" in ln:
            ages.append((section, m.group(2)))
        t = re.search(r"portal twitch \[(\S+)\] .*jitter mean ([\d.]+) px, p50 [\d.]+, p95 ([\d.]+), p99 ([\d.]+), max ([\d.]+)", ln)
        if t:
            twitch.append(t.groups())
    t = re.match(r"twitch \[(\S+)\] .*jitter mean ([\d.]+) px, p50 [\d.]+, p95 ([\d.]+), p99 ([\d.]+), max ([\d.]+)", ln)
    if t:
        twitch.append(t.groups())

secs = []
for s, ok, txt in rows:
    if not secs or secs[-1][0] != s:
        secs.append([s, 0, 0, []])
    secs[-1][1 if ok else 2] += 1
    if not ok:
        secs[-1][3].append(txt[:230])
print("== asserts per section ==")
tot_p = sum(x[1] for x in secs); tot_f = sum(x[2] for x in secs)
for s, p, f, fails in secs:
    print(f"{s:28s} pass {p:3d}  FAIL {f:3d}")
    for x in fails:
        print("     - " + x)
print(f"TOTAL pass {tot_p} FAIL {tot_f}")
print("\n== twitch (px at 1600x900): label mean p95 p99 max ==")
for t in twitch:
    print("  %-34s %6s %6s %6s %6s" % t)
print("\n== frame times (no per-frame readback) ==")
for tag, line in prof:
    print(f"  {tag:28s} {line}")
