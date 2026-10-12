"""Testbed suite core (lead, 2026-10-03): test registry, script builder, layout helpers.

A test is a function decorated with @test. It receives a builder `t` (one per mode), emits the game script through `t`
(setup, the player's real inputs, markers) and returns a `check(r)` function. tools/tb/judge.py runs `check` on the
recorded trace after the run. Nothing is asserted in game, so one failure never stops the run, and a verdict can be
re-judged without running the game again.

Verdicts:
  PASS     every expectation held
  FAIL     the setup was right (every `r.require` held) and an expectation did not hold: a real gap in the game side
  BLOCKED  a `r.require` did not hold (the player never got where the test put it, the mechanic itself never moved...):
           the test says nothing about the feature, fix the test or the setup first
  NO TRACE the run never reached the test (crash, watchdog, an earlier death that reloaded the map)
"""
import json
import math
import os

HERE = os.path.dirname(os.path.abspath(__file__))
LAYOUT = json.load(open(os.path.join(HERE, "..", "cfg", "pc_testbed_layout.json")))
BAYS = LAYOUT["bays"]
FPS = 60  # `wait N` is N/60 s of wall clock

# SDL scancodes for Minecraft (pcb_mckey) and Portal 2 console buttons for Chell
KEYS = {
    "forward": ("26", "forward"), "back": ("22", "back"), "left": ("4", "moveleft"), "right": ("7", "moveright"),
    "jump": ("44", "jump"), "duck": ("225", "duck"), "sprint": ("224", None),
    "use": ("rmb", "use"), "attack": ("lmb", "attack"), "attack2": (None, "attack2"),
}

REGISTRY = []
TARGET_MAP = None  # tbtest --target <map>: tests that load another map from the running session (Minecraft already linked)


def test(bay, modes=("chell", "mc"), title="", needs=None, order=50, run="main"):
    """needs: what the test depends on that may not be built yet (e.g. '#32 faith plate'); shown next to a FAIL.
    order: lower runs earlier; lethal tests (Chell can die and reload the map) use 90+. run: which game run holds it."""
    def deco(fn):
        for m in modes:
            REGISTRY.append({"id": "%s.%s.%s" % (bay, fn.__name__, m), "bay": bay, "mode": m, "fn": fn, "title": title or fn.__doc__ or fn.__name__,
                             "needs": needs.get(m) if isinstance(needs, dict) else needs, "order": order, "run": run})
        return fn
    return deco


def ent(bay, name):
    return BAYS[bay]["entities"][name]


def spot(bay):
    s = BAYS[bay]["spot"]
    return s["origin"], s["yaw"]


def inside(bay):
    s = BAYS[bay]["inside"]
    return s["origin"], s["yaw"]


def into(bay):
    return BAYS[bay]["into_dir"]


def udir(bay):
    return BAYS[bay]["u_dir"]


def opening(bay):
    """A point on the bay's opening plane (the hall wall's outer face), from the layout's v convention."""
    s = BAYS[bay]["spot"]["origin"]
    d = into(bay)
    # tb_spot is 160 u in front of the opening's outer face (layout: 'tb_spot_<bay> is 160 u in front of the bay's opening')
    return [s[0] + d[0] * 160, s[1] + d[1] * 160]


def v_of(bay, p):
    """How far p is into the bay (u along into_dir from the opening face)."""
    o, d = opening(bay), into(bay)
    return (p[0] - o[0]) * d[0] + (p[1] - o[1]) * d[1]


def at_v(bay, v, u=0.0, z=8.0):
    """The point v u into the bay, u along its width from the bay's centre line."""
    o, d, w = opening(bay), into(bay), udir(bay)
    c = BAYS[bay]["spot"]["origin"]
    # the centre line passes through the spot; opening() is on it
    return [o[0] + d[0] * v + w[0] * u, o[1] + d[1] * v + w[1] * u, z]


def yaw_to(a, b):
    return math.degrees(math.atan2(b[1] - a[1], b[0] - a[0])) % 360


class T:
    """Script builder for one test in one mode."""

    def __init__(self, tid, bay, mode, run_tag):
        self.id, self.bay, self.mode, self.run_tag = tid, bay, mode, run_tag
        self.lines = []
        self.place_target = None
        self.ready_place = None
        self.shots = []

    # ---- raw
    def cmd(self, *cs):
        self.lines += list(cs)

    def wait(self, s):
        self.lines.append("wait %d" % max(1, int(round(s * FPS))))

    def until(self, cond, s):
        self.lines.append("waituntil %s %d" % (cond, int(s * FPS)))

    def event(self, label):
        self.lines.append("pcb_tb_event %s" % label)

    def shot(self, label):
        name = "tb_%s__%s__%s" % (self.run_tag, self.id, label)
        self.shots.append((label, name))
        self.lines.append("pcb_glhud_shot %s" % name)

    def collision_shot(self, label, mode=2):
        """The collision overlay (what Minecraft collides with, classified) as a screenshot + box list (.txt)."""
        name = "tb_%s__%s__%s" % (self.run_tag, self.id, label)
        self.shots.append((label, name))
        self.lines.append("pcb_collision_shot %s %d" % (name, mode))
        self.wait(4.5)

    # ---- recording
    def watch(self, label, cls, name, field="", typ="f"):
        self.lines.append("pcb_tb_watch %s %s %s %s %s" % (label, cls, name, field or "-", typ))

    def watch_origin(self, label, name, cls="*"):
        self.watch(label, cls, name, "-", "o")

    def watch_angles(self, label, name, cls="*"):
        """Local angles (m_angRotation): m_angAbsRotation is computed lazily and can read 0 on a turning mover."""
        self.watch(label, cls, name, "m_angRotation", "v")

    def watch_count(self, label, name):
        self.watch(label, "*", name, "-", "n")

    def watch_class(self, label, cls):
        self.watch(label, cls, "*", "-", "c")

    # ---- the bay
    def relay(self, which="go", settle=None):
        self.cmd("ent_fire %s trigger" % BAYS[self.bay]["relays"][which]["name"])
        self.wait(BAYS[self.bay]["settle_s"] if settle is None else settle)

    def fire(self, target, inp, val=""):
        self.cmd(("ent_fire %s %s %s" % (target, inp, val)).strip())

    # ---- the player
    def place(self, p, yaw, pitch=0, fall=8):
        """Put the player (this test's mode) at p facing yaw. The judge checks at 'ready' that it got there (else BLOCKED)."""
        self.place_target = {"p": list(p), "yaw": yaw, "fall": fall}
        if self.mode == "mc":
            self.cmd("pcb_mc_place %.1f %.1f %.1f %d %d" % (p[0], p[1], p[2], round(yaw), fall))
            self.until("mcplaced", 15)
            self.wait(0.5)
            self.cmd("pcb_setview %d %d" % (pitch, round(yaw)))
        else:
            self.cmd("setpos %.1f %.1f %.1f" % tuple(p), "setang %d %d 0" % (pitch, round(yaw)))
            self.wait(0.6)
            self.cmd("pcb_setview %d %d" % (pitch, round(yaw)))
        self.wait(0.3)

    def drop_in(self, p, yaw):
        """Put the player in the air at p (a drop); no placement check beyond the start point."""
        self.place_target = {"p": list(p), "yaw": yaw, "fall": 99999, "air": True}
        if self.mode == "mc":
            self.cmd("pcb_mc_place %.1f %.1f %.1f %d 99999" % (p[0], p[1], p[2], round(yaw)))
        else:
            self.cmd("setpos %.1f %.1f %.1f" % tuple(p), "setang 0 %d 0" % round(yaw))

    def look(self, pitch, yaw):
        self.cmd("pcb_setview %.1f %.1f" % (pitch, yaw))

    def face(self, target):
        self.cmd("pcb_face %s" % target)

    def key(self, k, down):
        mc, ch = KEYS[k]
        if self.mode == "mc":
            if mc is None:
                raise ValueError("no Minecraft key for " + k)
            self.cmd("pcb_mckey %s %d" % (mc, 1 if down else 0))
        else:
            if ch is None:
                raise ValueError("no Portal 2 button for " + k)
            self.cmd("%s%s" % ("+" if down else "-", ch))

    def hold(self, k, s):
        self.key(k, True)
        self.wait(s)
        self.key(k, False)

    def tap(self, k):
        self.key(k, True)
        self.wait(0.12)
        self.key(k, False)

    def lethal(self):
        """After a test that may have killed Chell: Portal 2 reloads the map; give it time before the next test."""
        if self.mode == "chell":
            self.wait(10.0)
            self.until("spawned", 30)

    def ready(self):
        """Marks the end of setup. The placement checked at 'ready' is the latest one before it."""
        self.ready_place = dict(self.place_target) if self.place_target else None
        self.event("ready")


def build_script(tests, run_tag, out_dir, start_mode="chell"):
    """One game script for the given tests (already sorted). Returns (lines, per-test info for the manifest)."""
    fix = tests[0]["run"] not in ("arrival", "campaign", "campaign_linked", "campaign_portals", "campaign_arrows", "campaign_stage")  # (start-of-map tests must not be disturbed)
    L = ["# testbed suite run %s (tools/tb), generated" % run_tag, "sv_cheats 1", "pcb_assert_soft 1", "pcb_tb_run %s" % out_dir,
         "pc_input_lock 1"]  # the real keyboard and mouse are ignored for the run: only this script drives the players
    if fix:
        L += ["wait 120", "waituntil link 3600", "wait 120"]
    # start-of-map runs: the first test begins on the level's first tick (the arrival car is already rising)
    if fix:
        L += [
         # the shared test world's player had max_health 1000 left from an old run (a screen of hearts): back to vanilla
         "bridge_mode mc", "waituntil mcmode 900", "waituntil mcalive 600", "pcb_mc_cmd attribute @p minecraft:max_health base set 20", "wait 30",
         "bridge_mode chell", "waituntil chellmode 900", "wait 30"]
    info = []
    mode = "chell" if fix else start_mode
    for tc in tests:
        t = T(tc["id"], tc["bay"], tc["mode"], run_tag)
        check = tc["fn"](t)
        L.append("# ---- %s" % tc["id"])
        L.append("pcb_tb_begin %s" % tc["id"])
        # every test sets its own mode (a stray J, a death or a reload can leave the other one), then waits for it;
        # Portal 2's health is only touched once Chell leads (while Minecraft leads it is the body's held health)
        L += ["bridge_mode %s" % tc["mode"], "waituntil %s 900" % ("mcmode" if tc["mode"] == "mc" else "chellmode"), "wait %d" % (60 if tc["mode"] != mode else 10)]
        mode = tc["mode"]
        if tc["mode"] == "mc":
            L += ["pcb_mc_reset_player", "waituntil mcalive 600", "wait 10"]
        else:
            L += ["pcb_noclip_set 0", "ent_fire !player SetHealth 100"]
        L.append("pcb_tb_event start")
        L += t.lines
        L += ["pcb_tb_event done", "pcb_tb_end"]
        # leave the bay as the next test expects it
        for k in ("forward", "back", "left", "right", "jump", "duck", "use", "attack"):
            mc, ch = KEYS[k]
            L.append(("pcb_mckey %s 0" % mc) if mode == "mc" else "-%s" % ch)
        if tc["bay"] in BAYS:
            L.append("ent_fire %s trigger" % BAYS[tc["bay"]]["relays"]["reset"]["name"])
        L.append("wait 30")
        info.append({"id": tc["id"], "bay": tc["bay"], "mode": tc["mode"], "title": tc["title"], "needs": tc["needs"], "place": t.ready_place, "shots": t.shots})
    L += ["wait 60", "pcb_run_summary", "quit"]
    return L, info
