#!/usr/bin/env python3
"""Finish a compiled pc_pit_flings.bsp: rewrite its pakfile lump.

usage: pack.py <compiled.bsp> <valve_original.bsp> <out.bsp> [--alias-script file.nut]

- keeps what vbsp/vrad packed (cubemap-patched materials, static prop
  vertex lighting sp_hdr_*.vhv, ...)
- replaces vbsp's placeholder cubemap textures with Valve's built ones from the
  original map (buildcubemaps needs the running game, and the cubemap origins
  are identical, so the files carry over unchanged under the new map name)
- carries over Valve's simpleworldmodel (renamed path) for parity
- adds scripts/vscripts/portalcraft/pc_map_alias.nut

The pakfile is a stored (uncompressed) zip, as Source requires. It is written
as the last lump, after everything else, with the header updated.
"""
import argparse
import io
import re
import struct
import zipfile

from bsp import Bsp, L_PAKFILE


def static_props(b):
    ver, d = b.game_lump()["sprp"]
    n, = struct.unpack_from("<i", d, 0)
    names = [d[4 + i * 128:4 + i * 128 + 128].split(b"\0")[0] for i in range(n)]
    o = 4 + n * 128
    nl, = struct.unpack_from("<i", d, o)
    o += 4 + nl * 2
    npr, = struct.unpack_from("<i", d, o)
    o += 4
    size = (len(d) - o) // npr
    out = []
    for k in range(npr):
        r = d[o + k * size:o + (k + 1) * size]
        pose = tuple(round(x, 2) + 0.0 for x in struct.unpack_from("<6f", r, 0))
        out.append((names[struct.unpack_from("<H", r, 24)[0]], pose, r[30:31]))
    return out


def same_static_props(a, b):
    return static_props(a) == static_props(b)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("compiled")
    ap.add_argument("original")
    ap.add_argument("out")
    ap.add_argument("--alias-script", default=None)
    ap.add_argument("--compiled-name", default=None,
                    help="map name vbsp compiled under (default: compiled file name)")
    a = ap.parse_args()

    data = bytearray(open(a.compiled, "rb").read())
    comp = Bsp(a.compiled)
    orig = Bsp(a.original)
    new_name = re.sub(r"\.bsp$", "", a.out.split("/")[-1])
    old_name = re.sub(r"\.bsp$", "", a.original.split("/")[-1])
    comp_name = a.compiled_name or re.sub(r"\.bsp$", "", a.compiled.split("/")[-1])

    files = {}
    for info in comp.pakfile().infolist():
        n = info.filename.replace(comp_name, new_name)
        body = comp.pakfile().read(info.filename)
        if n.lower().endswith(".vmt"):
            body = body.replace(("maps/" + comp_name).encode(), ("maps/" + new_name).encode())
        files[n] = body
    oz = orig.pakfile()
    carried = []
    for info in oz.infolist():
        n = info.filename
        low = n.lower()
        is_cube = re.match(r"materials/maps/%s/(c-?\d+_-?\d+_-?\d+|cubemapdefault)(\.hdr)?\.vtf$" % old_name, low)
        is_swm = "simpleworldmodel" in low
        if is_cube:
            nn = n.replace("maps/" + old_name, "maps/" + new_name)
            files[nn] = oz.read(n)
            carried.append(nn)
        elif is_swm:
            # model files move to the new map's path (the engine looks there);
            # the materials keep their path, which the .mdl/.vmt reference
            nn = n.replace("models/maps/%s/simpleworldmodel" % old_name,
                           "models/maps/%s/simpleworldmodel" % new_name) if low.startswith("models/") else n
            files[nn] = oz.read(n)
            carried.append(nn)
    # static prop vertex lighting: same props in the same order as Valve's
    # (checked below), so Valve's per-prop sp_hdr_N.vhv files replace ours
    if same_static_props(comp, orig):
        for info in oz.infolist():
            if re.match(r"sp_(hdr_)?\d+\.vhv$", info.filename):
                files[info.filename] = oz.read(info.filename)
                carried.append(info.filename)
    else:
        print("static props differ from Valve's; keeping our vertex lighting")
    if a.alias_script:
        files["scripts/vscripts/portalcraft/pc_map_alias.nut"] = open(a.alias_script, "rb").read()

    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_STORED) as z:
        for n in sorted(files):
            zi = zipfile.ZipInfo(n, date_time=(2026, 1, 1, 0, 0, 0))
            zi.compress_type = zipfile.ZIP_STORED
            z.writestr(zi, files[n])
    pak = buf.getvalue()

    # drop the old pakfile lump if it is the last thing in the file, else just
    # append the new one (the old bytes become unreferenced)
    o, l, v, c = struct.unpack_from("<iii4s", data, 8 + L_PAKFILE * 16)
    end = len(data)
    if o + l == end:
        del data[o:]
    while len(data) % 4:
        data.append(0)
    new_o = len(data)
    data += pak
    struct.pack_into("<iii4s", data, 8 + L_PAKFILE * 16, new_o, len(pak), 0, b"\0\0\0\0")
    open(a.out, "wb").write(data)
    print("pakfile: %d files (%d carried from %s), %d bytes" % (len(files), len(carried), old_name, len(pak)))


if __name__ == "__main__":
    main()
