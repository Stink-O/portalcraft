#!/usr/bin/env python3
"""Side-by-side parity sheet (bridge #18): Minecraft's own view next to Portal 2's at the same pose.

  tools/parity_sheet.py <out.png> <label>=<png> [<label>=<png> ...]

Each image is scaled to 800 px wide and laid out two per row with its label on top. Typical pair:
  mc=cache/bridge/mcnative/mcnative_a18_p1.png  ours=cache/bridge/glhud_a18_p1_new.png
"""
import sys
from PIL import Image, ImageDraw

W = 800
args = sys.argv[1:]
if len(args) < 2:
    sys.exit(__doc__)
out, items = args[0], []
for a in args[1:]:
    label, path = a.split("=", 1)
    try:
        im = Image.open(path).convert("RGB")
    except OSError:
        im = Image.new("RGB", (W, 450), (60, 0, 60))
        label += " (missing)"
    im = im.resize((W, int(im.height * W / im.width)))
    items.append((label, im))
cols = 2
rows = (len(items) + cols - 1) // cols
h = max(im.height for _, im in items) + 24
sheet = Image.new("RGB", (W * cols, h * rows), (20, 20, 20))
d = ImageDraw.Draw(sheet)
for i, (label, im) in enumerate(items):
    x, y = (i % cols) * W, (i // cols) * h
    sheet.paste(im, (x, y + 24))
    d.text((x + 6, y + 5), label, fill=(255, 255, 255))
sheet.save(out)
print(out)
