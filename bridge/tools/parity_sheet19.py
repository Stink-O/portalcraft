#!/usr/bin/env python3
"""Side-by-side parity sheet (bridge #19; #18 has its own parity_sheet.py): Minecraft's own view vs Portal 2's frame at the same pose.
usage: parity_sheet19.py <out.png> <label>=<minecraft.png>,<portal2.png> [<label>=...]...
Each row: Minecraft native (left), ours in the chamber (right), both scaled to 800 px wide, labelled."""
import sys
from PIL import Image, ImageDraw

W = 800
rows = []
for arg in sys.argv[2:]:
    label, pair = arg.split('=', 1)
    a, b = pair.split(',')
    ims = []
    for p in (a, b):
        try:
            im = Image.open(p).convert('RGB')
            im = im.resize((W, int(im.size[1] * W / im.size[0])), Image.LANCZOS)
        except Exception as e:  # a missing shot: a grey placeholder
            im = Image.new('RGB', (W, 450), (60, 60, 60))
            ImageDraw.Draw(im).text((10, 10), 'missing: %s' % p, fill='white')
        ims.append(im)
    rows.append((label, ims))
h = sum(max(i.size[1] for i in ims) + 24 for _, ims in rows)
sheet = Image.new('RGB', (2 * W + 8, h), (20, 20, 20))
d = ImageDraw.Draw(sheet)
y = 0
for label, ims in rows:
    d.text((6, y + 5), '%s   (left: Minecraft native, right: Portal 2 chamber render)' % label, fill='white')
    y += 24
    for c, im in enumerate(ims):
        sheet.paste(im, (c * (W + 8), y))
    y += max(i.size[1] for i in ims)
sheet.save(sys.argv[1])
print(sys.argv[1], sheet.size)
