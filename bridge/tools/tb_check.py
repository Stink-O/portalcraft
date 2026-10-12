#!/usr/bin/env python3
"""Bridge #29: check that every entity of pc_testbed's layout JSON is in the plugin's map index.

    tools/tb_check.py [mapindex.txt] [layout.json]     (default cache/bridge/mapindex/pc_testbed.txt)
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
idx = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "cache/bridge/mapindex/pc_testbed.txt")
lay = sys.argv[2] if len(sys.argv) > 2 else os.path.join(HERE, "cfg", "pc_testbed_layout.json")
d = json.load(open(lay))
text = open(idx).read().split()
have = set()
for line in open(idx):
    parts = line.split()
    if len(parts) > 2 and parts[0] == "ent":
        have.add(parts[1])
want = []
for n in d["order"]:
    b = d["bays"][n]
    for en, e in b["entities"].items():
        if e.get("classname") not in ("func_instance", "brush"):
            want.append((n, en))
    want += [(n, b["relays"]["go"]["name"]), (n, b["relays"]["reset"]["name"]), (n, b["spot"]["name"])]
missing = [(b, e) for b, e in want if e not in have]
print("layout entities: %d, in the index: %d, missing: %d" % (len(want), len(want) - len(missing), len(missing)))
for b, e in missing:
    print("  MISSING %-10s %s" % (b, e))
sys.exit(1 if missing else 0)
