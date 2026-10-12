#!/usr/bin/env python3
"""Print pc_testbed's bay table (markdown) from the layout JSON, for maps/README.md."""
import json, sys
d = json.load(open(sys.argv[1] if len(sys.argv) > 1 else "../build/pc_testbed_layout.json"))
print("| bay | side/slot | interior min .. max (x y z) | standing spot | main entity | go / reset relay | tb_ names |")
print("|---|---|---|---|---|---|---|")
for n in d["order"]:
    b = d["bays"][n]
    i = b["interior"]
    sp = b["spot"]
    names = ", ".join(k for k in b["entities"] if k != b["main"])
    print("| %s | %s%d%s | %d %d %d .. %d %d %d | `%s` (%d %d %d, yaw %d) | `%s` | `%s` / `%s` | %s |" % (
        n, b["side"], b["slot"], "+%d" % (b["nslots"] - 1) if b["nslots"] > 1 else "", *i["min"], *i["max"], sp["name"], *[round(x) for x in sp["origin"]], sp["yaw"],
        b["main"], b["relays"]["go"]["name"], b["relays"]["reset"]["name"], names))
