"""Minimal type-preserving NBT reader/writer (gzip'd files), for tools/world78/migrate.py (bridge #78). Values are (tag_type, value)."""
import gzip
import struct

END, BYTE, SHORT, INT, LONG, FLOAT, DOUBLE, BYTE_ARRAY, STRING, LIST, COMPOUND, INT_ARRAY, LONG_ARRAY = range(13)


class _R:
    def __init__(self, b):
        self.b, self.i = b, 0

    def take(self, n):
        v = self.b[self.i:self.i + n]
        if len(v) != n:
            raise ValueError("truncated NBT")
        self.i += n
        return v

    def u(self, fmt):
        return struct.unpack(">" + fmt, self.take(struct.calcsize(">" + fmt)))[0]

    def s(self):
        return self.take(self.u("H")).decode("utf-8", "surrogatepass")


def _read(r, t):
    if t == BYTE: return r.u("b")
    if t == SHORT: return r.u("h")
    if t == INT: return r.u("i")
    if t == LONG: return r.u("q")
    if t == FLOAT: return r.u("f")
    if t == DOUBLE: return r.u("d")
    if t == BYTE_ARRAY: return r.take(r.u("i"))
    if t == STRING: return r.s()
    if t == LIST:
        et = r.u("B"); n = r.u("i")
        return (et, [_read(r, et) for _ in range(n)])
    if t == COMPOUND:
        o = {}
        while True:
            tt = r.u("B")
            if tt == END: return o
            k = r.s()
            o[k] = (tt, _read(r, tt))
    if t == INT_ARRAY:
        n = r.u("i"); return list(struct.unpack(">%di" % n, r.take(4 * n)))
    if t == LONG_ARRAY:
        n = r.u("i"); return list(struct.unpack(">%dq" % n, r.take(8 * n)))
    raise ValueError("bad tag %d" % t)


def _w(out, t, v):
    if t == BYTE: out.append(struct.pack(">b", v))
    elif t == SHORT: out.append(struct.pack(">h", v))
    elif t == INT: out.append(struct.pack(">i", v))
    elif t == LONG: out.append(struct.pack(">q", v))
    elif t == FLOAT: out.append(struct.pack(">f", v))
    elif t == DOUBLE: out.append(struct.pack(">d", v))
    elif t == BYTE_ARRAY: out.append(struct.pack(">i", len(v))); out.append(bytes(v))
    elif t == STRING: b = v.encode("utf-8", "surrogatepass"); out.append(struct.pack(">H", len(b))); out.append(b)
    elif t == LIST:
        et, items = v; out.append(struct.pack(">Bi", et, len(items)))
        for x in items: _w(out, et, x)
    elif t == COMPOUND:
        for k, (tt, vv) in v.items():
            out.append(struct.pack(">B", tt)); _w(out, STRING, k); _w(out, tt, vv)
        out.append(b"\0")
    elif t == INT_ARRAY: out.append(struct.pack(">i%di" % len(v), len(v), *v))
    elif t == LONG_ARRAY: out.append(struct.pack(">i%dq" % len(v), len(v), *v))
    else: raise ValueError("bad tag %d" % t)


def load(path):
    raw = open(path, "rb").read()
    try:
        raw = gzip.decompress(raw)
    except OSError:
        pass
    r = _R(raw)
    t = r.u("B"); name = r.s()
    v = _read(r, t)
    return name, (t, v)


def dumps(name, tv):
    out = []
    t, v = tv
    out.append(struct.pack(">B", t)); _w(out, STRING, name); _w(out, t, v)
    return gzip.compress(b"".join(out))
