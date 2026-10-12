#!/usr/bin/env python3
"""Finish a compiled pc_testbed.bsp: add the map-name alias script to its pakfile.

usage: pack_testbed.py <compiled.bsp> <out.bsp> [--alias-script file.nut]
Keeps everything vbsp/vrad packed; the pakfile is rewritten as a stored zip at the end of the file.
"""
import argparse
import io
import struct
import zipfile

from bsp import Bsp, L_PAKFILE


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("compiled")
    ap.add_argument("out")
    ap.add_argument("--alias-script", default=None)
    a = ap.parse_args()
    data = bytearray(open(a.compiled, "rb").read())
    comp = Bsp(a.compiled)
    files = {i.filename: comp.pakfile().read(i.filename) for i in comp.pakfile().infolist()}
    if a.alias_script:
        files["scripts/vscripts/portalcraft/pc_map_alias.nut"] = open(a.alias_script, "rb").read()
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_STORED) as z:
        for n in sorted(files):
            zi = zipfile.ZipInfo(n, date_time=(2026, 1, 1, 0, 0, 0))
            zi.compress_type = zipfile.ZIP_STORED
            z.writestr(zi, files[n])
    pak = buf.getvalue()
    o, l, v, c = struct.unpack_from("<iii4s", data, 8 + L_PAKFILE * 16)
    if o + l == len(data):
        del data[o:]
    while len(data) % 4:
        data.append(0)
    new_o = len(data)
    data += pak
    struct.pack_into("<iii4s", data, 8 + L_PAKFILE * 16, new_o, len(pak), 0, b"\0\0\0\0")
    open(a.out, "wb").write(data)
    print("pakfile: %d files, %d bytes" % (len(files), len(pak)))


if __name__ == "__main__":
    main()
