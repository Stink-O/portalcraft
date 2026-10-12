"""Minimal Portal 2 (VBSP v21) reader for the decompiler.

Only the lumps the BSP->VMF converter needs. Layouts follow the public
Source SDK bspfile.h structures (the same ones crates/source_assets parses).
Read-only: never writes to the input file.
"""
import io
import lzma
import struct
import zipfile

L_ENTITIES, L_PLANES, L_TEXDATA, L_VERTEXES = 0, 1, 2, 3
L_NODES, L_TEXINFO, L_FACES = 5, 6, 7
L_LEAFS, L_FACEIDS, L_EDGES, L_SURFEDGES, L_MODELS = 10, 11, 12, 13, 14
L_LEAFBRUSHES, L_BRUSHES, L_BRUSHSIDES = 17, 18, 19
L_AREAS, L_AREAPORTALS = 20, 21
L_DISPINFO, L_ORIGFACES, L_DISP_VERTS = 26, 27, 33
L_GAME_LUMP, L_PAKFILE, L_CLIPPORTALVERTS, L_CUBEMAPS = 35, 40, 41, 42
L_TEXDATA_STRING_DATA, L_TEXDATA_STRING_TABLE, L_OVERLAYS = 43, 44, 45
L_DISP_TRIS = 48
L_WORLDLIGHTS, L_WORLDLIGHTS_HDR = 15, 54
L_FACES_HDR = 58
L_OVERLAY_FADES, L_OVERLAY_SYSTEM_LEVELS = 60, 61

CONTENTS_SOLID = 0x1
CONTENTS_WINDOW = 0x2
CONTENTS_GRATE = 0x8
CONTENTS_WATER = 0x20
CONTENTS_DETAIL = 0x8000000
CONTENTS_TRANSLUCENT = 0x10000000


def _lzma(data):
    if data[:4] != b"LZMA":
        return data
    usize, csize = struct.unpack_from("<II", data, 4)
    props = data[12:17]
    dec = lzma.LZMADecompressor(
        lzma.FORMAT_RAW,
        filters=[lzma._decode_filter_properties(lzma.FILTER_LZMA1, props)])
    return dec.decompress(data[17:17 + csize])[:usize]


class Bsp:
    def __init__(self, path):
        self.path = path
        self.data = open(path, "rb").read()
        d = self.data
        assert d[:4] == b"VBSP", "not a VBSP file"
        self.version = struct.unpack_from("<i", d, 4)[0]
        self.lumps = []
        for i in range(64):
            o, l, v, c = struct.unpack_from("<iii4s", d, 8 + i * 16)
            self.lumps.append((o, l, v, c))
        self.map_revision = struct.unpack_from("<i", d, 8 + 64 * 16)[0]
        self._parse()

    def lump(self, i):
        o, l, v, c = self.lumps[i]
        if l == 0:
            return b""
        return _lzma(self.data[o:o + l])

    def lump_version(self, i):
        return self.lumps[i][2]

    def _arr(self, i, fmt):
        b = self.lump(i)
        n = struct.calcsize(fmt)
        return [struct.unpack_from(fmt, b, k * n) for k in range(len(b) // n)]

    def _parse(self):
        self.planes = [(p[0:3], p[3], p[4]) for p in self._arr(L_PLANES, "<4fi")]
        self.verts = self._arr(L_VERTEXES, "<3f")
        self.edges = self._arr(L_EDGES, "<2H")
        self.surfedges = [s[0] for s in self._arr(L_SURFEDGES, "<i")]
        # texinfo: textureVecs[2][4], lightmapVecs[2][4], flags, texdata
        self.texinfo = []
        for t in self._arr(L_TEXINFO, "<16f2i"):
            self.texinfo.append({
                "tv": (t[0:4], t[4:8]), "lv": (t[8:12], t[12:16]),
                "flags": t[16], "texdata": t[17]})
        sd = self.lump(L_TEXDATA_STRING_DATA)
        tab = [s[0] for s in self._arr(L_TEXDATA_STRING_TABLE, "<i")]
        names = [sd[o:sd.index(b"\0", o)].decode("latin-1") for o in tab]
        # texdata: reflectivity(3f), nameStringTableID, width, height, view_w, view_h
        self.texdata = []
        for t in self._arr(L_TEXDATA, "<3f5i"):
            self.texdata.append({"name": names[t[3]], "w": t[4], "h": t[5]})
        # face (56 bytes): planenum H, side B, onNode B, firstedge i, numedges h,
        # texinfo h, dispinfo h, surfaceFogVolumeID h, styles 4B, lightofs i,
        # area f, lmmins 2i, lmsize 2i, origFace i, numPrims H, firstPrimID H,
        # smoothingGroups I
        fmt = "<HBBihhhh4Bif2i2iiHHI"
        def faces(i):
            return [dict(zip(("planenum", "side", "onnode", "firstedge", "numedges",
                              "texinfo", "dispinfo", "fog", "s0", "s1", "s2", "s3",
                              "lightofs", "area", "lm0", "lm1", "ls0", "ls1",
                              "origface", "nprims", "firstprim", "smoothing"), f))
                    for f in self._arr(i, fmt)]
        self.faces = faces(L_FACES)
        self.origfaces = faces(L_ORIGFACES)
        self.faceids = [f[0] for f in self._arr(L_FACEIDS, "<H")]
        self.brushes = [dict(zip(("firstside", "numsides", "contents"), b))
                        for b in self._arr(L_BRUSHES, "<3i")]
        self.brushsides = [dict(zip(("planenum", "texinfo", "dispinfo", "bevel", "thin"), s))
                           for s in self._arr(L_BRUSHSIDES, "<Hhhbb")]
        # dmodel: mins, maxs, origin, headnode, firstface, numfaces
        self.models = [dict(mins=m[0:3], maxs=m[3:6], origin=m[6:9], headnode=m[9],
                            firstface=m[10], numfaces=m[11])
                       for m in self._arr(L_MODELS, "<9f3i")]
        # node (32): planenum i, children 2i, mins 3h, maxs 3h, firstface H,
        # numfaces H, area h, pad h
        self.nodes = [dict(planenum=n[0], children=(n[1], n[2]))
                      for n in self._arr(L_NODES, "<3i6h2H2h")]
        # leaf v1 (32): contents i, cluster h, area:9 flags:7 h, mins 3h, maxs 3h,
        # firstleafface H, numleaffaces H, firstleafbrush H, numleafbrushes H,
        # leafWaterDataID h, pad h
        self.leafs = [dict(contents=l[0], cluster=l[1], area=l[2] & 0x1ff,
                           firstleafbrush=l[11], numleafbrushes=l[12])
                      for l in self._arr(L_LEAFS, "<ihh6hHHHHhh")]
        self.leafbrushes = [b[0] for b in self._arr(L_LEAFBRUSHES, "<H")]
        self.areas = self._arr(L_AREAS, "<2i")
        # dareaportal: portalKey H, otherarea H, firstClipPortalVert H,
        # numClipPortalVerts H, planenum i
        self.areaportals = [dict(zip(("key", "otherarea", "firstvert", "numverts", "planenum"), a))
                            for a in self._arr(L_AREAPORTALS, "<4Hi")]
        self.clipportalverts = self._arr(L_CLIPPORTALVERTS, "<3f")
        self.cubemaps = [(c[0:3], c[3]) for c in self._arr(L_CUBEMAPS, "<4i")]
        self.dispverts = [(v[0:3], v[3], v[4]) for v in self._arr(L_DISP_VERTS, "<5f")]
        self.disptris = [t[0] for t in self._arr(L_DISP_TRIS, "<H")]
        self._parse_dispinfo()
        self.entities_text = self.lump(L_ENTITIES).rstrip(b"\0").decode("latin-1")

    def _parse_dispinfo(self):
        b = self.lump(L_DISPINFO)
        self.dispinfo = []
        for k in range(len(b) // 176):
            o = k * 176
            sp = struct.unpack_from("<3f", b, o)
            vs, ts, power, mintess = struct.unpack_from("<4i", b, o + 12)
            smooth, contents = struct.unpack_from("<fi", b, o + 28)
            mapface, = struct.unpack_from("<H", b, o + 36)
            allowed = struct.unpack_from("<10I", b, o + 176 - 40)
            self.dispinfo.append(dict(start=sp, vertstart=vs, tristart=ts, power=power,
                                      mintess=mintess, smooth=smooth, contents=contents,
                                      mapface=mapface, allowed=allowed))

    def face_winding(self, f):
        pts = []
        for k in range(f["numedges"]):
            se = self.surfedges[f["firstedge"] + k]
            e = self.edges[abs(se)]
            pts.append(self.verts[e[0] if se >= 0 else e[1]])
        return pts

    def game_lump(self):
        b = self.lump(L_GAME_LUMP)
        n, = struct.unpack_from("<i", b, 0)
        out = {}
        for k in range(n):
            gid, flags, ver, ofs, ln = struct.unpack_from("<4sHHii", b, 4 + k * 16)
            data = self.data[ofs:ofs + ln]
            if flags & 1:
                data = _lzma(data)
            out[gid[::-1].decode("latin-1")] = (ver, data)
        return out

    def pakfile(self):
        o, l, v, c = self.lumps[L_PAKFILE]
        return zipfile.ZipFile(io.BytesIO(self.data[o:o + l]))

    def worldlights(self, hdr=True):
        i = L_WORLDLIGHTS_HDR if hdr else L_WORLDLIGHTS
        b = self.lump(i)
        out = []
        if self.lump_version(i) == 1:
            n = 100
            for k in range(len(b) // n):
                v = struct.unpack_from("<12f3i7f3i", b, k * n)
                out.append(dict(origin=v[0:3], intensity=v[3:6], normal=v[6:9],
                                shadow_offset=v[9:12], cluster=v[12], type=v[13],
                                style=v[14], stopdot=v[15], stopdot2=v[16],
                                exponent=v[17], radius=v[18], constant=v[19],
                                linear=v[20], quadratic=v[21], flags=v[22],
                                texinfo=v[23], owner=v[24]))
        else:
            n = 88
            for k in range(len(b) // n):
                v = struct.unpack_from("<9f3i7f3i", b, k * n)
                out.append(dict(origin=v[0:3], intensity=v[3:6], normal=v[6:9],
                                shadow_offset=(0, 0, 0), cluster=v[9], type=v[10],
                                style=v[11], stopdot=v[12], stopdot2=v[13],
                                exponent=v[14], radius=v[15], constant=v[16],
                                linear=v[17], quadratic=v[18], flags=v[19],
                                texinfo=v[20], owner=v[21]))
        return out


def parse_entities(text):
    """Entity lump -> list of [(key, value), ...] preserving order and duplicates."""
    ents, cur, i, n = [], None, 0, len(text)
    while i < n:
        c = text[i]
        if c == "{":
            cur = []
            i += 1
        elif c == "}":
            ents.append(cur)
            cur = None
            i += 1
        elif c == '"':
            j = text.index('"', i + 1)
            k = text[i + 1:j]
            a = text.index('"', j + 1)
            b = text.index('"', a + 1)
            cur.append((k, text[a + 1:b]))
            i = b + 1
        else:
            i += 1
    return ents
