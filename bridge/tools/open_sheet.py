#!/usr/bin/env python3
"""Frame-by-frame open-animation sheet from pcb_frame_dump crops.
usage: open_sheet.py <run_dir> <out.png> <label,label,...> [crop_half_px=70]
Each row is a dump label; each column is Portal 2's open amount (the first dumped frame at or past it,
from the dump's log line), the frame before the shot first. The crop is centred on the crosshair (the
screen centre of the dump box), where the shot lands."""
import os, re, sys
from PIL import Image, ImageDraw
run, out, labels = sys.argv[1], sys.argv[2], sys.argv[3].split(',')
half = int(sys.argv[4]) if len(sys.argv) > 4 else 70
log = open(os.path.join(run, 'bridge.log'), errors='replace').read()
targets = [None, 0.05, 0.15, 0.25, 0.35, 0.45, 0.55, 0.65, 0.75, 0.85, 0.95, 1.0, 'end']
tw, th = 140, 105
sheet = Image.new('RGB', (tw * len(targets), th * len(labels)), 'black')
d = ImageDraw.Draw(sheet)
for r, lab in enumerate(labels):
    fr = [(int(a), float(b), int(c), int(e)) for a, b, x, c, e in re.findall(r'frame dump %s (\d+) \(host frame \d+\): portal open vars ([\d.]+) ([\d.]+), set (\d) (\d)' % re.escape(lab), log)]
    if not fr: continue
    act = next((i for i, f in enumerate(fr) if f[2] == 1 and f[1] < 0.5), 0)  # (the material still holds the old portal's 1.0 for a frame)
    path = lambda k: os.path.join(run, 'frames', '%s_%03d.png' % (lab, k))
    last = Image.open(path(fr[-1][0]))
    cx, cy = last.size[0] // 2, last.size[1] // 2  # the crosshair: the shot lands there
    box = (cx - half, cy - int(half * 0.75), cx + half, cy + int(half * 0.75))
    for c, t in enumerate(targets):
        if t is None: k = max(0, act - 1)
        elif t == 'end': k = len(fr) - 1
        else: k = next((i for i in range(act, len(fr)) if fr[i][2] == 1 and fr[i][1] >= t), None)
        if k is None: continue
        im = Image.open(path(fr[k][0])).convert('RGB').crop(box).resize((tw, th), Image.NEAREST)
        sheet.paste(im, (c * tw, r * th))
        d.text((c * tw + 3, r * th + 2), '%s o%.2f' % (lab.split('_', 1)[-1], fr[k][1] if fr[k][2] else 0), fill='yellow')
sheet.save(out)
print(out)
