"""Testbed suite judge: reads a run's traces, runs each test's check, writes results.json and report.html."""
import html
import json
import math
import os


class Blocked(Exception):
    pass


class Trace:
    def __init__(self, path):
        self.frames, self.events, self.hurts, self.end = [], {}, [], None
        for line in open(path):
            line = line.strip()
            if not line:
                continue
            try:
                d = json.loads(line)
            except ValueError:
                continue
            if "t" in d and "m" in d:
                self.frames.append(d)
            elif "ev" in d:
                self.events.setdefault(d["ev"], d["t"])
            elif "hurt" in d:
                self.hurts.append(d["hurt"])
            elif "end" in d:
                self.end = d["end"]


def pos(f):
    """The player's feet: Minecraft's while Minecraft leads, else Portal 2's."""
    return f["mc"]["f"] if f.get("m") == "mc" and "mc" in f else f["p2"]["o"]


def hdist(a, b):
    return math.hypot(a[0] - b[0], a[1] - b[1])


class R:
    """What a check sees: the trace plus helpers. require -> BLOCKED, expect -> FAIL."""

    def __init__(self, trace, info):
        self.tr, self.info = trace, info
        self.mode = info["mode"]
        self.checks = []  # (kind, ok, text)

    # ---- verdict helpers
    def require(self, ok, text):
        self.checks.append(("require", bool(ok), text))
        if not ok:
            raise Blocked(text)

    def expect(self, ok, text):
        self.checks.append(("expect", bool(ok), text))
        return ok

    def log(self, pattern, after=None):
        """bridge.log lines of this test (between its TB BEGIN and TB END) that contain pattern; with after=<event>,
        only the lines after that event's marker."""
        lines = self.info.get("_log", [])
        if after:
            k = next((i for i, l in enumerate(lines) if "TB EVENT " in l and l.rstrip().endswith(" " + after)), len(lines))
            lines = lines[k:]
        return [l for l in lines if pattern in l]

    def mclog(self, pattern, after=None, before=None):
        """(bridge #48) Minecraft's own log (mc.log of the run) lines that contain pattern. The log has no test markers, so a test marks
        its sections with a harmless server command, `pcb_mc_cmd tag @p add tb_<name>` (the mod logs "test command: tag @p add tb_<name>"),
        and asks for the lines after one marker and before another."""
        lines = self.info.get("_mc", [])
        def mark(name):
            return next((i for i, l in enumerate(lines) if "test command: tag @p add %s" % name in l), None)
        a = mark(after) if after else 0
        b = mark(before) if before else len(lines)
        if a is None or b is None:
            return []
        return [l for l in lines[a:b] if pattern in l]

    def note(self, text):
        self.checks.append(("note", True, text))

    # ---- time
    def t(self, ev):
        if ev is None:
            return None
        if ev not in self.tr.events:
            raise Blocked("the script never reached '%s' (run cut short?)" % ev)
        return self.tr.events[ev]

    def span(self, a=None, b=None):
        ta, tb = self.t(a) if a else -1, self.t(b) if b else 1e18
        return [f for f in self.tr.frames if ta <= f["t"] <= tb]

    def at(self, ev):
        t0 = self.t(ev)
        for f in self.tr.frames:
            if f["t"] >= t0:
                return f
        raise Blocked("no frame after '%s'" % ev)

    def last(self, a=None, b=None):
        s = self.span(a, b)
        if not s:
            raise Blocked("no frames between %s and %s" % (a, b))
        return s[-1]

    # ---- the player
    def pos(self, f):
        return pos(f)

    def mode_ok(self, ev="ready"):
        f = self.at(ev)
        self.require(f["m"] == self.mode, "the player is in %s mode at '%s' (is %s)" % (self.mode, ev, f["m"]))

    def placed(self, tol=12, ev="ready"):
        """The placement in this test landed: right mode, within tol horizontally, not below the allowed fall."""
        pt = self.info.get("place")
        self.mode_ok(ev)
        if not pt:
            return
        f = self.at(ev)
        p = pos(f)
        want = pt["p"]
        d = hdist(p, want)
        dz = p[2] - want[2]
        ok = d <= tol and (pt.get("air") or (-pt["fall"] - 4 <= dz <= 6))
        self.require(ok, "placed at (%.0f %.0f %.0f), wanted (%.0f %.0f %.0f): %.0f u off, dz %.0f" % (p[0], p[1], p[2], want[0], want[1], want[2], d, dz))

    def path(self, a=None, b=None):
        return [pos(f) for f in self.span(a, b)]

    def rise(self, a, b=None):
        p = self.path(a, b)
        return max(q[2] for q in p) - p[0][2] if p else 0

    def drop(self, a, b=None):
        p = self.path(a, b)
        return p[0][2] - min(q[2] for q in p) if p else 0

    def speed_h(self, a, b=None):
        """The highest horizontal speed (u/s) between frames at least 0.05 s apart (works at any frame rate)."""
        s = self.span(a, b)
        best = 0
        j = 0
        for i in range(1, len(s)):
            while j + 1 < i and s[i]["t"] - s[j + 1]["t"] >= 0.05:
                j += 1
            dt = s[i]["t"] - s[j]["t"]
            if dt >= 0.02:
                d = hdist(pos(s[i]), pos(s[j]))
                if d / dt < 3000:  # a teleport is not speed
                    best = max(best, d / dt)
        return best

    def jumps(self, a=None, b=None, min_u=96):
        """Discontinuities (portal transits, teleports): consecutive frames more than min_u apart."""
        s = self.span(a, b)
        out = []
        for i in range(1, len(s)):
            d = math.dist(pos(s[i - 1]), pos(s[i]))
            if d > min_u:
                out.append((s[i]["t"], pos(s[i - 1]), pos(s[i])))
        return out

    def airborne_drops(self, a=None, b=None, fall_speed=550.0):
        """Falls of the Minecraft player: stretches with its ground flag off whose downward speed passed fall_speed u/s
        (free fall accelerates past it within a few hundred u; riding a car up or down, even held off its floor, keeps a
        steady car speed, about 100-400 u/s in Valve's elevators). Returns (start t, drop u, peak down speed), worst first."""
        out, cur = [], None
        fs = [f for f in self.span(a, b) if "mc" in f]
        for i, f in enumerate(fs):
            z = f["mc"]["f"][2]
            if not f["mc"]["g"]:
                if cur is None:
                    cur = {"t": f["t"], "top": z, "low": z, "vmax": 0.0}
                cur["top"], cur["low"] = max(cur["top"], z), min(cur["low"], z)
                j = i
                while j > 0 and f["t"] - fs[j - 1]["t"] < 0.3:  # over 0.3 s: positions arrive in uneven steps
                    j -= 1
                if j < i and f["t"] - fs[j]["t"] > 0.2:
                    cur["vmax"] = max(cur["vmax"], (fs[j]["mc"]["f"][2] - z) / (f["t"] - fs[j]["t"]))
            elif cur is not None:
                out.append(cur)
                cur = None
        if cur is not None:
            out.append(cur)
        falls = [(c["t"], c["top"] - c["low"], c["vmax"]) for c in out if c["vmax"] > fall_speed and c["top"] - c["low"] > 40]
        return sorted(falls, key=lambda x: -x[1])

    def handover(self):
        """The level start in Minecraft mode: (seconds from the first real frame until Minecraft leads, the jump of the
        player's position on that frame in u: Portal 2's body just before vs Minecraft's feet just after), or None."""
        fs = [f for f in self.tr.frames if max(abs(c) for c in pos(f)) > 1]
        for i in range(1, len(fs)):
            if fs[i]["m"] == "mc" and fs[i - 1]["m"] != "mc" and "mc" in fs[i]:
                return fs[i]["t"] - fs[0]["t"], math.dist(fs[i - 1]["p2"]["o"], fs[i]["mc"]["f"])
        return None

    def ground_frac(self, a, b=None):
        s = [f for f in self.span(a, b) if "mc" in f]
        return sum(f["mc"]["g"] for f in s) / len(s) if s else None

    # ---- damage
    def hp(self, f):
        return f["p2"]["hp"]

    def damage(self, a=None, b=None):
        """Damage taken in [a, b]. Chell: health lost on Portal 2's side. Minecraft: the mod's hurt events (whole test:
        mc.log has no sub-second clock, so the window is the test), as (count, total, sources)."""
        if self.mode == "mc":
            src = {}
            for h in self.tr.hurts:
                src[h["src"]] = src.get(h["src"], 0) + 1
            return len(self.tr.hurts), sum(min(h["dmg"], 1e6) for h in self.tr.hurts), src
        s = self.span(a, b)
        lost, prev = 0, None
        for f in s:
            h = f["p2"]["hp"]
            if prev is not None and h < prev:
                lost += prev - h
            prev = h
        return (1 if lost else 0), lost, {}

    # ---- watched values
    def w(self, f, label):
        return f.get("w", {}).get(label)

    def wseries(self, label, a=None, b=None):
        return [(f["t"], f.get("w", {}).get(label)) for f in self.span(a, b)]

    def portals(self, f):
        """Every prop_portal: (two, active, [x, y, z], linkage)."""
        return [(p[0], p[1], p[2:5], p[5]) for p in f["por"]]

    def portal_near(self, f, two, point, tol=72):
        """An open portal of that colour within tol of the point: its distance, else None."""
        ds = [math.dist(p[2], point) for p in self.portals(f) if p[0] == two and p[1] == 1]
        ds = [d for d in ds if d <= tol]
        return min(ds) if ds else None


def judge_run(run_dir, tests_by_id, manifest):
    import core  # noqa: F401  (tests module imported by the caller)
    results = []
    # each test's slice of bridge.log (the run's copy holds earlier runs too: start at this run's script load)
    logs, cur = {}, None
    lp = os.path.join(run_dir, "bridge.log")
    if os.path.exists(lp):
        lines = open(lp, errors="replace").read().splitlines()
        start = max([i for i, l in enumerate(lines) if "script: loaded" in l and run_dir in l] or [0])
        for l in lines[start:]:
            if "TB BEGIN " in l:
                cur = l.split("TB BEGIN ", 1)[1].strip()
                logs[cur] = []
            if cur:
                logs[cur].append(l)
            if "TB END " in l:
                cur = None
    mcl = os.path.join(run_dir, "mc.log")
    mc_lines = open(mcl, errors="replace").read().splitlines() if os.path.exists(mcl) else []
    for info in manifest["tests"]:
        info = dict(info, _log=logs.get(info["id"], []), _mc=mc_lines)
        tid = info["id"]
        path = os.path.join(run_dir, tid + ".jsonl")
        res = {"id": tid, "bay": info["bay"], "mode": info["mode"], "title": info["title"], "needs": info["needs"], "checks": [], "shots": []}
        for label, name in info["shots"]:
            for cand in (os.path.join(run_dir, name + ".png"),):
                if os.path.exists(cand):
                    res["shots"].append([label, os.path.basename(cand)])
        if not os.path.exists(path):
            res["verdict"] = "NO TRACE"
            res["why"] = "the run never reached this test"
            results.append(res)
            continue
        tr = Trace(path)
        r = R(tr, info)
        tc = tests_by_id.get(tid)
        if tc is None:  # renamed or removed since the run
            continue
        t = core.T(tid, tc["bay"], tc["mode"], manifest["run"])
        check = tc["fn"](t)
        try:
            if "start" not in tr.events:
                raise Blocked("the test never started (mode switch or player reset hung)")
            r.placed() if info.get("place") else r.mode_ok("start")
            check(r)
            fails = [c for c in r.checks if c[0] == "expect" and not c[1]]
            res["verdict"] = "FAIL" if fails else "PASS"
            res["why"] = "; ".join(c[2] for c in fails)
        except Blocked as e:
            res["verdict"] = "BLOCKED"
            res["why"] = str(e)
        except Exception as e:  # a bug in the check itself
            res["verdict"] = "BLOCKED"
            res["why"] = "check error: %r" % e
        res["checks"] = r.checks
        res["dur"] = tr.end
        res["plot"] = plot(tr)
        results.append(res)
    return results


def plot(tr):
    """A small SVG: player height (blue) and horizontal distance from the start (grey) over time, events as ticks."""
    fs = tr.frames
    if len(fs) < 2:
        return ""
    t0, t1 = fs[0]["t"], fs[-1]["t"]
    p0 = pos(fs[0])
    zs = [pos(f)[2] for f in fs]
    hs = [hdist(pos(f), p0) for f in fs]
    zlo, zhi = min(zs), max(zs)
    hhi = max(hs) or 1
    W, H = 360, 90

    def X(t):
        return 4 + (W - 8) * (t - t0) / max(1e-6, t1 - t0)

    def Yz(z):
        return H - 6 - (H - 14) * (z - zlo) / max(1.0, zhi - zlo)

    def Yh(h):
        return H - 6 - (H - 14) * h / hhi
    step = max(1, len(fs) // 300)
    pz = " ".join("%.1f,%.1f" % (X(f["t"]), Yz(z)) for f, z in list(zip(fs, zs))[::step])
    ph = " ".join("%.1f,%.1f" % (X(f["t"]), Yh(h)) for f, h in list(zip(fs, hs))[::step])
    ev = "".join('<line x1="%.1f" x2="%.1f" y1="0" y2="%d" class="ev"/><text x="%.1f" y="9" class="evt">%s</text>' % (X(t), X(t), H, X(t) + 2, html.escape(k)) for k, t in tr.events.items() if t0 <= t <= t1)
    return ('<svg viewBox="0 0 %d %d" class="plot"><polyline points="%s" class="ph"/><polyline points="%s" class="pz"/>%s'
            '<text x="%d" y="%d" class="lab">z %.0f..%.0f u, h-dist max %.0f u, %.1f s</text></svg>') % (W, H, ph, pz, ev, W - 4, H - 2, zlo, zhi, hhi, t1 - t0)


def report(results, run, out):
    order = ["FAIL", "BLOCKED", "NO TRACE", "PASS"]
    counts = {k: sum(1 for r in results if r["verdict"] == k) for k in order}
    bays = []
    for r in results:
        if r["bay"] not in bays:
            bays.append(r["bay"])
    rows = []
    for b in bays:
        cells = []
        for m in ("chell", "mc"):
            rs = [r for r in results if r["bay"] == b and r["mode"] == m]
            cells.append("".join('<a href="#%s" class="pill %s" title="%s">%s</a>' % (r["id"], r["verdict"].replace(" ", ""), html.escape(r["title"]), html.escape(r["id"].split(".")[1])) for r in rs) or '<span class="na">-</span>')
        rows.append("<tr><th>%s</th><td>%s</td><td>%s</td></tr>" % (b, cells[0], cells[1]))
    cards = []
    for r in sorted(results, key=lambda r: (order.index(r["verdict"]), r["id"])):
        ck = "".join('<li class="%s%s">%s</li>' % (c[0], "" if c[1] else " bad", html.escape(c[2])) for c in r["checks"])
        shots = "".join('<figure><img loading="lazy" src="%s"><figcaption>%s</figcaption></figure>' % (html.escape(s[1]), html.escape(s[0])) for s in r["shots"])
        cards.append('<section id="%s" class="card"><h3><span class="pill %s">%s</span> %s <small>%s</small></h3><p class="title">%s%s</p>%s<ul>%s</ul><div class="shots">%s</div></section>' % (
            r["id"], r["verdict"].replace(" ", ""), r["verdict"], r["id"], r["mode"], html.escape(r["title"]),
            (' <span class="needs">needs %s</span>' % html.escape(r["needs"])) if r["needs"] and r["verdict"] != "PASS" else "",
            r.get("plot", ""), ck, shots) + ("<p class='why'>%s</p>" % html.escape(r["why"]) if r.get("why") and r["verdict"] != "PASS" else "") )
    css = """
:root{--bg:#f6f7f8;--fg:#1c1f23;--mut:#6b7280;--card:#fff;--line:#e3e5e8;--pass:#1f8a4c;--fail:#c2352b;--blk:#b7791f;--nt:#6b7280;--z:#2563eb;--h:#9ca3af}
@media (prefers-color-scheme:dark){:root:not([data-theme=light]){--bg:#15171a;--fg:#e8eaed;--mut:#9aa0a6;--card:#1d2024;--line:#2c3036;--pass:#3fb36f;--fail:#ef6b5f;--blk:#e0a64a;--z:#6aa1ff;--h:#6b7280}}
:root[data-theme=dark]{--bg:#15171a;--fg:#e8eaed;--mut:#9aa0a6;--card:#1d2024;--line:#2c3036;--pass:#3fb36f;--fail:#ef6b5f;--blk:#e0a64a;--z:#6aa1ff;--h:#6b7280}
body{background:var(--bg);color:var(--fg);font:14px/1.45 system-ui,sans-serif;margin:0;padding:16px;max-width:1100px;margin:auto}
h1{font-size:20px;margin:4px 0 2px}.sum{color:var(--mut);margin-bottom:14px}
table{border-collapse:collapse;width:100%;background:var(--card);border:1px solid var(--line);border-radius:8px}
th,td{padding:5px 8px;border-bottom:1px solid var(--line);text-align:left;vertical-align:top}th{font-weight:600;width:110px}
.pill{display:inline-block;padding:1px 7px;margin:1px 3px 1px 0;border-radius:10px;font-size:12px;color:#fff;text-decoration:none}
.PASS{background:var(--pass)}.FAIL{background:var(--fail)}.BLOCKED{background:var(--blk)}.NOTRACE{background:var(--nt)}
.card{background:var(--card);border:1px solid var(--line);border-radius:8px;padding:10px 12px;margin:12px 0}
.card h3{margin:0 0 4px;font-size:15px}.card small{color:var(--mut);font-weight:400}.title{margin:0 0 6px;color:var(--mut)}
.needs{color:var(--blk)}ul{margin:6px 0;padding-left:18px}li.require{color:var(--mut)}li.bad{color:var(--fail);font-weight:600}li.note{color:var(--mut);font-style:italic}
.why{color:var(--fail)}.plot{width:100%;max-width:520px;height:auto;display:block;background:var(--bg);border-radius:6px}
.plot .pz{fill:none;stroke:var(--z);stroke-width:1.5}.plot .ph{fill:none;stroke:var(--h);stroke-width:1}.plot .ev{stroke:var(--line)}
.plot .evt{font-size:7px;fill:var(--mut)}.plot .lab{font-size:8px;fill:var(--mut);text-anchor:end}
.shots{display:flex;flex-wrap:wrap;gap:8px}figure{margin:0;max-width:340px}figure img{width:100%;border-radius:6px}figcaption{font-size:12px;color:var(--mut)}
.na{color:var(--mut)}@media(max-width:600px){th{width:70px}}
"""
    page = ('<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Testbed results</title><style>%s</style></head><body>'
            '<h1>pc_testbed suite: run %s</h1><div class="sum">%s</div><p class="sum">PASS works. FAIL = set up correctly, the feature misbehaved. BLOCKED = the test could not set up '
            '(says nothing about the feature). Blue line = player height, grey = distance from the start.</p>'
            '<table><tr><th>bay</th><th>Chell</th><th>Minecraft player</th></tr>%s</table>%s</body></html>') % (
        css, html.escape(run), " · ".join("%d %s" % (counts[k], k) for k in order if counts[k]), "".join(rows), "".join(cards))
    open(out, "w").write(page)
