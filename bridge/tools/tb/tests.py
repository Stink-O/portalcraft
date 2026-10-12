"""The pc_testbed tests. Each one: put the player somewhere (verified), use the mechanic with real inputs, and judge the
recorded effect, with the cause checked too (the mechanic really moved, the damage happened inside the hazard...).
Positions come from the layout JSON (cfg/pc_testbed_layout.json), never typed in."""
import math

from core import BAYS, at_v, ent, inside, into, test, udir, v_of


# ------------------------------------------------------------------------------------------------ riding movers
def _ride(t, bay, name, stand, travel_axis, travel, wait_s, extra_watch=None):
    """Stand on a mover, start it, ride. travel_axis: 'z' or 'v' (into the bay)."""
    t.watch_origin("mover", name)
    t.relay("reset", settle=BAYS[bay]["settle_s"])
    t.place(stand, BAYS[bay]["spot"]["yaw"])
    t.ready()
    t.shot("before")
    t.relay("go", settle=wait_s)
    t.event("moved")
    t.shot("after")

    def check(r):
        m0, m1 = r.w(r.at("ready"), "mover"), r.w(r.last("ready", "moved"), "mover")
        r.require(m0 is not None, "the mover %s exists" % name)
        if travel_axis == "z":
            mv = m1[2] - m0[2]
            r.require(mv >= travel * 0.8, "the mover itself rose %.0f u (expected about %d)" % (mv, travel))
            p0, p1 = r.pos(r.at("ready")), r.pos(r.last("ready", "moved"))
            got = p1[2] - p0[2]
            r.expect(got >= mv - 24, "the player was carried up %.0f u with it (mover %.0f)" % (got, mv))
        else:
            mv = v_of(bay, m1) - v_of(bay, m0)
            r.require(mv >= travel * 0.8, "the mover itself moved %.0f u into the bay (expected about %d)" % (mv, travel))
            p0, p1 = r.pos(r.at("ready")), r.pos(r.last("ready", "moved"))
            got = v_of(bay, p1) - v_of(bay, p0)
            r.expect(got >= mv - 32, "the player was carried %.0f u with it (mover %.0f)" % (got, mv))
            r.expect(abs(p1[2] - p0[2]) < 24, "the player stayed on top (height change %.0f u)" % (p1[2] - p0[2]))
        r.expect(r.drop("ready", "moved") < 40, "the player never fell (lowest point %.0f u below the start)" % r.drop("ready", "moved"))
    return check


@test("lift", title="stand on the lift platform, start it, ride it 192 u up")
def ride(t):
    e = ent("lift", "tb_lift")
    return _ride(t, "lift", "tb_lift", [e["center"][0], e["center"][1], e["top"] + 2], "z", e["travel"], 4.5)


@test("mlinear", title="stand on the sliding platform, ride it 192 u into the bay")
def ride(t):  # noqa: F811
    e = ent("mlinear", "tb_movelinear")
    return _ride(t, "mlinear", "tb_movelinear", [e["center"][0], e["center"][1], e["top"] + 2], "v", e["travel"], 4.5)


# ------------------------------------------------------------------------------------------------ doors that block
def _door(t, bay, door, open_):
    """Walk at the doorway with the door closed (blocked) or open (pass)."""
    part_v = v_of(bay, ent(bay, door).get("center") or ent(bay, door)["origin"])
    o, yaw = inside(bay)
    t.relay("reset")
    if open_:
        t.relay("go")
    t.place(o, yaw)
    t.ready()
    t.hold("forward", 3.0)
    t.event("walked")
    t.shot("end")

    def check(r):
        p0 = r.pos(r.at("ready"))
        vs = [v_of(bay, p) for p in r.path("ready", "walked")]
        r.require(max(vs) - vs[0] > 60, "the player walked (%.0f u towards the door)" % (max(vs) - vs[0]))
        if open_:
            r.expect(max(vs) > part_v + 48, "went through the open doorway: reached %.0f u into the bay, the door is at %.0f" % (max(vs), part_v))
        else:
            r.expect(max(vs) < part_v, "stopped by the closed door: reached %.0f u into the bay, the door is at %.0f" % (max(vs), part_v))
    return check


@test("doorh", title="walk into the closed sliding door: blocked")
def closed(t):
    return _door(t, "doorh", "tb_door_h", False)


@test("doorh", title="open the sliding door, walk through")
def opened(t):
    return _door(t, "doorh", "tb_door_h", True)


@test("train", title="stand on the train, start it, ride to the far end")
def ride(t):  # noqa: F811
    e = ent("train", "tb_train")
    return _ride(t, "train", "tb_train", [e["center"][0], e["center"][1], e["top"] + 2], "v", 384, 9.0)


@test("doorpanel", title="stand on the floor panel, it rises 128 u (parented plates)")
def ride(t):  # noqa: F811
    e = ent("doorpanel", "tb_doorpanel_lift")
    m = ent("doorpanel", "tb_doorpanel_model")["origin"]
    return _ride(t, "doorpanel", "tb_doorpanel_lift", [m[0], m[1], 2], "z", e["travel"], 4.5)


@test("rotate", title="stand off-centre on the turntable, start it: carried round with it")
def ride(t):  # noqa: F811
    e = ent("rotate", "tb_turntable")
    pv = e["pivot"]
    u = udir("rotate")
    stand = [pv[0] + u[0] * 64, pv[1] + u[1] * 64, e["top"] + 2]
    t.watch_angles("ang", "tb_turntable")
    t.relay("reset")
    t.place(stand, BAYS["rotate"]["spot"]["yaw"])
    t.ready()
    t.relay("go", settle=6.0)
    t.event("moved")
    t.shot("after")

    def check(r):
        a0, a1 = r.w(r.at("ready"), "ang"), r.w(r.last("ready", "moved"), "ang")
        r.require(a0 is not None, "the turntable exists")
        turned = (a1[1] - a0[1] + 540) % 360 - 180
        r.require(abs(turned) >= 30, "the turntable itself turned %.0f deg" % turned)
        p0, p1 = r.pos(r.at("ready")), r.pos(r.last("ready", "moved"))
        b0 = math.degrees(math.atan2(p0[1] - pv[1], p0[0] - pv[0]))
        b1 = math.degrees(math.atan2(p1[1] - pv[1], p1[0] - pv[0]))
        orbit = (b1 - b0 + 540) % 360 - 180
        tol = 15 if r.mode == "mc" else 40  # Chell is Portal 2's own physics: she slides outward a little (the reference)
        r.expect(abs(orbit - turned) < tol, "the player went round the pivot %.0f deg (the disc turned %.0f, tolerance %d)" % (orbit, turned, tol))
        d1 = math.hypot(p1[0] - pv[0], p1[1] - pv[1])
        r.expect(abs(d1 - 64) < (24 if r.mode == "mc" else 40), "the player stayed at its spot on the disc (%.0f u from the pivot, started 64)" % d1)
        r.expect(r.drop("ready", "moved") < 24, "the player never fell (dropped %.0f u)" % r.drop("ready", "moved"))
    return check


# (lead, 2026-10-03) "tilt.ride" retired: a func_door_rotating turning under a standing player is blocked by Source's
# pusher rules (Chell too, full4-full6), so it measured nothing. Valve's moving panels are animated arms: tilt.ramp.


@test("departure", title="stand in the departure car, start it: carried up the shaft", order=80)
def ride(t):  # noqa: F811
    car = ent("departure", "departure_elevator")["origin"]
    t.watch_origin("car", "departure_elevator-elevator_1")
    t.relay("reset", settle=5.0)
    t.place([car[0], car[1], 8], (BAYS["departure"]["spot"]["yaw"] + 180) % 360)
    t.ready()
    t.relay("go", settle=9.0)
    t.event("moved")
    t.shot("after")

    def check(r):
        c0, c1 = r.w(r.at("ready"), "car"), r.w(r.last("ready", "moved"), "car")
        r.require(c0 is not None, "the car exists")
        r.require(c1[2] - c0[2] > 100, "the car itself rose %.0f u" % (c1[2] - c0[2]))
        got = r.pos(r.last("ready", "moved"))[2] - r.pos(r.at("ready"))[2]
        r.expect(got > (c1[2] - c0[2]) - 40, "the player rose %.0f u with the car (car %.0f)" % (got, c1[2] - c0[2]))
    return check


# ------------------------------------------------------------------------------------------------ doors (prop door)
@test("doors", title="walk into the closed test chamber door: blocked")
def closed(t):  # noqa: F811
    return _door(t, "doors", "tb_door", False)


@test("doors", title="open the test chamber door, walk through")
def opened(t):  # noqa: F811
    return _door(t, "doors", "tb_door", True)


# ------------------------------------------------------------------------------------------------ launchers
@test("faith", title="walk onto the armed faith plate: launched, land at its target", needs={"mc": "#32 faith plate"})
def launch(t):
    tgt = ent("faith", "tb_faith_target")["origin"]
    plate = ent("faith", "tb_faith")
    t.relay("go")
    t.place(at_v("faith", 8), BAYS["faith"]["spot"]["yaw"])
    t.ready()
    t.key("forward", True)
    t.wait(0.8)
    t.key("forward", False)
    t.wait(2.7)
    t.event("landed")
    t.shot("landed")

    def check(r):
        pv0 = v_of("faith", plate["center"]) - plate["size"][0] / 2
        vs = [v_of("faith", p) for p in r.path("ready", "landed")]
        r.require(max(vs) >= pv0, "the player walked onto the plate (reached %.0f u in, the plate starts at %.0f)" % (max(vs), pv0))
        r.expect(r.rise("ready", "landed") >= 50, "launched: rose %.0f u (the plate's arc is about 75 u high)" % r.rise("ready", "landed"))
        end = r.pos(r.last("ready", "landed"))
        d = math.hypot(end[0] - tgt[0], end[1] - tgt[1])
        r.expect(d <= 128, "landed %.0f u from tb_faith_target (at (%.0f %.0f %.0f))" % (d, end[0], end[1], end[2]))
    return check


@test("funnel", title="walk into the excursion funnel: carried up the beam", needs={"mc": "#32 funnel"})
def carry(t):
    f = ent("funnel", "tb_funnel")["origin"]
    t.relay("go")
    t.place(at_v("funnel", 150), BAYS["funnel"]["spot"]["yaw"])
    t.ready()
    t.hold("forward", 0.7)
    t.wait(3.0)
    t.event("carried")
    t.shot("in_beam")

    def check(r):
        path = r.path("ready", "carried")
        near = [p for p in path if math.hypot(p[0] - f[0], p[1] - f[1]) < 96]
        r.require(near, "the player reached the beam (closest %.0f u from its axis)" % min(math.hypot(p[0] - f[0], p[1] - f[1]) for p in path))
        rise = max(p[2] for p in near) - path[0][2]
        r.expect(rise >= 120, "carried up the beam: rose %.0f u while within 96 u of its axis" % rise)
        end = r.pos(r.last("ready", "carried"))
        r.expect(math.hypot(end[0] - f[0], end[1] - f[1]) < 72, "held on the axis (%.0f u off at the end)" % math.hypot(end[0] - f[0], end[1] - f[1]))
    return check


@test("gel", title="jump on blue gel: bounces higher than a normal jump", needs={"mc": "gel (not assigned)"})
def bounce(t):
    g = ent("gel", "tb_gel_floor")["origin"]
    t.relay("go", settle=5.0)
    # off the gel (placing onto blue gel bounces the player before 'ready'), then walk onto it and jump
    t.place([g[0] - 150 * into("gel")[0], g[1] - 150 * into("gel")[1], 8], BAYS["gel"]["spot"]["yaw"])
    t.ready()
    t.key("forward", True)
    t.wait(0.75)
    t.tap("jump")
    t.key("forward", False)
    t.wait(3.0)
    t.event("bounced")

    def check(r):
        r.expect(r.rise("ready", "bounced") >= 90, "rose %.0f u (a normal jump is about 45-50)" % r.rise("ready", "bounced"))
    return check


# ------------------------------------------------------------------------------------------------ light bridge
@test("bridge", title="stand and walk on the light bridge over the pit", needs={"mc": "#32 light bridge"})
def walk(t):
    b = ent("bridge", "tb_bridge")["origin"]
    pit = ent("bridge", "tb_bridge_pit")["origin"]
    t.watch("on", "prop_wall_projector", "tb_bridge", "m_bEnabled", "b")
    t.relay("go")
    t.place([b[0], pit[1], 44], (BAYS["bridge"]["spot"]["yaw"] + 90) % 360, fall=400)
    t.ready()
    t.wait(1.0)
    t.hold("forward", 0.5)
    t.wait(1.0)
    t.event("walked")
    t.shot("on_bridge")

    def check(r):
        r.note("projector enabled at ready: %s" % r.w(r.at("ready"), "on"))
        low = min(p[2] for p in r.path("ready", "walked"))
        r.expect(low > 20, "stayed on the bridge (lowest %.0f u; the bridge is at 40, the pit floor at %.0f)" % (low, pit[2]))
    return check


# ------------------------------------------------------------------------------------------------ portals
@test("portals", title="walk into a wall portal: come out of its partner, still walking")
def walk(t):  # noqa: F811
    a = ent("portals", "tb_pair_a")
    b = ent("portals", "tb_pair_b")
    t.watch("a", "prop_portal", "tb_pair_a", "m_bActivated", "b")
    t.relay("go", settle=1.5)
    n = a["normal"]
    t.place([a["origin"][0] + n[0] * 96, a["origin"][1] + n[1] * 96, 8], (math.degrees(math.atan2(-n[1], -n[0]))) % 360)
    t.ready()
    t.hold("forward", 1.3)
    t.wait(0.5)
    t.event("walked")
    t.shot("after")

    def check(r):
        r.require(r.w(r.at("ready"), "a") == 1, "tb_pair_a is open at ready")
        js = r.jumps("ready", "walked")
        r.expect(js, "a portal transit happened (a jump in position)")
        if js:
            tj, p_in, p_out = js[0]
            da = math.hypot(p_in[0] - a["origin"][0], p_in[1] - a["origin"][1])
            db = math.hypot(p_out[0] - b["origin"][0], p_out[1] - b["origin"][1])
            r.expect(da < 80 and db < 80, "entered at tb_pair_a (%.0f u) and came out at tb_pair_b (%.0f u)" % (da, db))
            r.expect(r.speed_h(None, None) > 80, "kept walking after the transit (%.0f u/s)" % r.speed_h(None, None))
    return check


@test("portals", modes=("chell",), title="Chell spawns with the dual portal gun: shoot both colours; white panels take portals, black metal does not", order=2)
def shoot(t):
    t.watch_class("guns", "weapon_portalgun")
    o, yaw = [-800.0, -1280, 8], 270
    t.event("gun_check")
    # no `give`: the map must hand her the dual gun itself, as Valve's maps do
    t.place(o, yaw)
    t.ready()
    t.face("tb_panel_wall_back")
    t.wait(0.3)
    t.tap("attack")
    t.wait(1.0)
    t.event("blue_back")
    t.face("tb_nop_wall_back")
    t.wait(0.3)
    t.tap("attack2")
    t.wait(1.0)
    t.event("orange_black")
    t.face("tb_panel_floor")
    t.wait(0.3)
    t.tap("attack2")
    t.wait(1.0)
    t.event("orange_floor")
    t.shot("portals")
    # the gun reuses the map's linkage-0 portals (the fling pair): close everything again
    t.cmd("ent_fire prop_portal SetActivatedState 0")
    W, NW, FL = ent("portals", "tb_panel_wall_back")["origin"], ent("portals", "tb_nop_wall_back")["origin"], ent("portals", "tb_panel_floor")["origin"]

    def check(r):
        r.note("weapon_portalgun entities: %s" % r.w(r.at("gun_check"), "guns"))
        d = r.portal_near(r.last("ready", "blue_back"), 0, W)
        r.expect(d is not None, "the spawn gun fires blue: a blue portal on the white back panel (%s)" % ("%.0f u from its centre" % d if d is not None else "none there"))
        d = r.portal_near(r.last("ready", "orange_black"), 1, NW)
        r.expect(d is None, "no orange portal on the black metal (%s)" % ("one %.0f u from it" % d if d is not None else "none"))
        d = r.portal_near(r.last("ready", "orange_floor"), 1, FL)
        r.expect(d is not None, "the spawn gun fires orange: an orange portal on the white floor panel (%s)" % ("%.0f u from its centre" % d if d is not None else "none there"))
    return check


@test("fling", title="drop into the floor portal from 600 u: flung out of the wall portal")
def fling(t):
    fp = ent("fling", "tb_fling_floor_portal")["origin"]
    wp = ent("fling", "tb_fling_wall_portal")
    t.watch("fp", "prop_portal", "tb_fling_floor_portal", "m_bActivated", "b")
    t.relay("go", settle=1.5)
    t.event("pre")
    t.drop_in([fp[0], fp[1], 600], 0)
    t.wait(0.2)
    t.ready()
    t.wait(3.5)
    t.event("flung")
    t.shot("after")

    def check(r):
        r.require(r.w(r.at("pre"), "fp") == 1, "the floor portal is open")
        p = r.pos(r.at("ready"))
        r.require(p[2] > 300, "the player was dropped from high up (z %.0f at ready)" % p[2])
        js = r.jumps("ready", "flung")
        r.expect(js, "fell through the floor portal (a transit)")
        if js:
            tj, _, p_out = js[0]
            d = math.hypot(p_out[0] - wp["origin"][0], p_out[1] - wp["origin"][1])
            r.expect(d < 96, "came out of the wall portal (%.0f u from it)" % d)
            frames = [f for f in r.span("ready", "flung") if f["t"] >= tj]
            exit_v = max((math.hypot(*f["p2"]["v"][:2]) for f in frames[:6]), default=0)
            sp = r.speed_h("ready", "flung")
            r.expect(max(sp, exit_v) > 600, "flung out fast: %.0f u/s horizontally (measured from the path), %.0f u/s (Portal 2 velocity) just after the exit; the fall reached about 850" % (sp, exit_v))
            far = max(math.hypot(r.pos(f)[0] - wp["origin"][0], r.pos(f)[1] - wp["origin"][1]) for f in frames)
            r.note("went %.0f u from the wall portal (the bay is 256 u wide)" % far)
    return check


# ------------------------------------------------------------------------------------------------ buttons and cubes
def _cube(t):
    """A fresh cube on the floor (every cube dissolved first; the template drops one 224 u)."""
    t.relay("reset", settle=1.5)
    t.fire("tb_cube_template", "ForceSpawn")
    t.wait(3.0)


@test("cubes", title="walk onto the floor button: it presses (counter +1)", needs={"mc": "#32 use (puppet body on buttons)"})
def floor_button(t):
    b = ent("cubes", "tb_btn_floor")["origin"]
    d = into("cubes")
    t.watch("cnt", "math_counter", "tb_cnt_floor", "m_OutValue", "f")
    t.relay("reset", settle=1.5)
    t.place([b[0] - d[0] * 110, b[1] - d[1] * 110, 8], BAYS["cubes"]["spot"]["yaw"])
    t.ready()
    t.hold("forward", 0.6)
    t.wait(1.0)
    t.event("on")

    def check(r):
        c0, c1 = r.w(r.at("ready"), "cnt"), r.w(r.last("ready", "on"), "cnt")
        r.require(c0 is not None, "the counter tb_cnt_floor exists")
        end = r.pos(r.last("ready", "on"))
        r.require(math.hypot(end[0] - b[0], end[1] - b[1]) < 48, "the player ended on the button (%.0f u from it)" % math.hypot(end[0] - b[0], end[1] - b[1]))
        r.expect(c1 > c0, "the button pressed: counter %s -> %s" % (c0, c1))
    return check


@test("cubes", title="use the pedestal button: it presses (counter +1)", needs={"mc": "#32 use / right-click"})
def pedestal(t):
    b = ent("cubes", "tb_btn_pedestal")["origin"]
    d = into("cubes")
    t.watch("cnt", "math_counter", "tb_cnt_pedestal", "m_OutValue", "f")
    t.relay("reset", settle=1.5)
    t.place([b[0] - d[0] * 64, b[1] - d[1] * 64, 8], BAYS["cubes"]["spot"]["yaw"])
    t.ready()
    t.face("tb_btn_pedestal")
    t.wait(0.3)
    t.tap("use")
    t.wait(1.0)
    t.event("used")
    t.shot("used")

    def check(r):
        c0, c1 = r.w(r.at("ready"), "cnt"), r.w(r.last("ready", "used"), "cnt")
        r.require(c0 is not None, "the counter tb_cnt_pedestal exists")
        r.expect(c1 > c0, "the button pressed: counter %s -> %s" % (c0, c1))
    return check


@test("cubes", title="pick up a cube, put it on the floor button: it presses", needs={"mc": "#32 use (pick up cubes)"})
def cube_on_button(t):
    b = ent("cubes", "tb_btn_floor")["origin"]
    c = ent("cubes", "tb_cube")["origin"]
    d = into("cubes")
    t.watch("cnt", "math_counter", "tb_cnt_floor", "m_OutValue", "f")
    t.watch_origin("cube", "tb_cube")
    _cube(t)
    t.place([c[0] - d[0] * 72, c[1] - d[1] * 72, 8], BAYS["cubes"]["spot"]["yaw"])
    t.ready()
    t.face("tb_cube")
    t.wait(0.3)
    t.tap("use")
    t.wait(1.0)
    t.event("picked")
    # carry it: walk sideways to stand beside the button (the held cube goes with the player) and drop it on the button
    t.place([b[0] - d[0] * 72, b[1] - d[1] * 72, 8], BAYS["cubes"]["spot"]["yaw"], pitch=35)
    t.wait(0.8)
    t.tap("use")
    t.wait(2.0)
    t.event("dropped")
    t.shot("dropped")

    def check(r):
        r.require(r.w(r.at("ready"), "cube") is not None, "a cube is there")
        r.expect(any(f["p2"]["hold"] for f in r.span("picked", "dropped")[:20]), "picked the cube up")
        cube = r.w(r.last("ready", "dropped"), "cube")
        dc = math.hypot(cube[0] - b[0], cube[1] - b[1])
        r.expect(dc < 40, "the cube rests on the button (%.0f u from its centre)" % dc)
        c0, c1 = r.w(r.at("ready"), "cnt"), r.w(r.last("ready", "dropped"), "cnt")
        r.expect(c1 > c0, "the button pressed: counter %s -> %s" % (c0, c1))
    return check


# ------------------------------------------------------------------------------------------------ fizzler
@test("fizzler", title="walk through the fizzler: the player's own (gun) portals close; a map pair in another group stays", needs={"mc": "#32 fizzler on the player"})
def portals(t):
    W, FL = ent("portals", "tb_panel_wall_back")["origin"], ent("portals", "tb_panel_floor")["origin"]
    t.watch("a", "prop_portal", "tb_pair_a", "m_bActivated", "b")
    t.watch("grid", "trigger_portal_cleanser", "tb_fizzler", "m_bDisabled", "b")
    t.relay("reset", settle=1.0)
    t.fire("tb_go_portals", "trigger")
    # the gun's pair (linkage group 0), as if shot: blue on the white back panel, orange on the white floor panel
    t.cmd("portal_place 0 0 %.1f %.1f %.1f 0 90 0" % (W[0], W[1] + 1, W[2]), "portal_place 0 1 %.1f %.1f %.1f -90 0 0" % (FL[0], FL[1], FL[2] + 1))
    t.wait(1.5)
    t.place(at_v("fizzler", 100, u=44), BAYS["fizzler"]["spot"]["yaw"])  # beside tb_fizz_cube (v 150), inside the 128 u doorway
    t.ready()
    t.hold("forward", 1.8)
    t.wait(0.5)
    t.event("through")
    t.fire("tb_reset_portals", "trigger")
    t.cmd("ent_fire prop_portal SetActivatedState 0")
    gv = v_of("fizzler", ent("fizzler", "tb_fizzler")["center"])

    def check(r):
        def gun_open(f):
            return sum(1 for p in r.portals(f) if p[3] == 0 and p[1] == 1)
        f = r.at("ready")
        r.require(gun_open(f) == 2, "both gun portals open before (%d)" % gun_open(f))
        r.require(r.w(f, "grid") == 0, "the grid is on")
        vs = [v_of("fizzler", p) for p in r.path("ready", "through")]
        r.require(max(vs) > gv + 24, "the player crossed the grid (reached %.0f u in, grid at %.0f)" % (max(vs), gv))
        e = r.last("ready", "through")
        r.expect(gun_open(e) == 0, "the gun portals closed (%d still open)" % gun_open(e))
        r.expect(r.w(f, "a") != 1 or r.w(e, "a") == 1, "the map's own pair (linkage group 1) stayed open (tb_pair_a %s -> %s)" % (r.w(f, "a"), r.w(e, "a")))
    return check


@test("fizzler", title="carry a cube through the fizzler: it dissolves", needs={"mc": "#32 use + fizzler"})
def cube(t):  # noqa: F811
    c = ent("fizzler", "tb_fizz_cube")["origin"]
    t.watch_count("cubes", "tb_fizz_cube")
    t.relay("reset", settle=1.0)
    t.place(at_v("fizzler", 60), BAYS["fizzler"]["spot"]["yaw"])
    t.ready()
    t.face("tb_fizz_cube")
    t.wait(0.3)
    t.tap("use")
    t.wait(0.8)
    t.event("picked")
    t.look(0, BAYS["fizzler"]["spot"]["yaw"])
    t.hold("forward", 2.2)
    t.wait(1.5)
    t.event("through")

    def check(r):
        r.require(r.w(r.at("ready"), "cubes") == 1, "the cube is there (%s; nothing respawns it after an earlier test dissolved it)" % r.w(r.at("ready"), "cubes"))
        held = any(f["p2"]["hold"] for f in r.span("picked", "through")[:30])
        r.expect(held, "picked the cube up")
        gv = v_of("fizzler", ent("fizzler", "tb_fizzler")["center"])
        vs = [v_of("fizzler", p) for p in r.path("picked", "through")]
        r.require(max(vs) > gv + 24, "the player crossed the grid (reached %.0f u in, grid at %.0f)" % (max(vs), gv))
        r.expect(r.w(r.last("ready", "through"), "cubes") == 0, "the cube dissolved (%s left)" % r.w(r.last("ready", "through"), "cubes"))
    return check


# ------------------------------------------------------------------------------------------------ hazards
@test("turret", title="stand in an asleep turret's view: no damage", order=40)
def asleep(t):
    tu = ent("turret", "tb_turret")["origin"]
    t.relay("reset")
    t.place([tu[0] - 300, tu[1], 8], 0)
    t.ready()
    t.wait(3.0)
    t.event("waited")

    def check(r):
        n, dmg, src = r.damage("ready", "waited")
        r.expect(n == 0, "not hurt (%d hits, %.0f damage %s)" % (n, dmg, src))
    return check


@test("turret", title="stand in an awake turret's view: it shoots the player", order=97)
def shoots(t):  # noqa: F811
    tu = ent("turret", "tb_turret")["origin"]
    t.watch("on", "npc_portal_turret_floor", "tb_turret", "m_bEnabled", "b")
    t.relay("go")
    t.place([tu[0] - 300, tu[1], 8], 0)
    t.ready()
    t.wait(2.0)
    t.event("shot")
    t.shot("turret")
    t.relay("reset", settle=0.5)
    t.lethal()

    def check(r):
        r.note("turret enabled flag at ready: %s" % r.w(r.at("ready"), "on"))
        n, dmg, src = r.damage("ready", "shot")
        r.expect(n >= 1, "hurt by the turret (%d hits, %.0f damage %s)" % (n, dmg, src))
    return check


@test("laser", title="stand in the laser beam: it hurts", order=88, needs={"mc": "#32 damage sources"})
def beam(t):
    em = ent("laser", "tb_laser_emitter")["origin"]
    t.watch("caught", "math_counter", "tb_cnt_catcher", "m_OutValue", "f")
    t.relay("reset", settle=1.0)
    t.place([em[0], ent("laser", "tb_laser_relay")["origin"][1], 8], 0)
    t.ready()
    t.relay("go", settle=0.1)
    t.event("on")
    t.wait(2.5)
    t.event("burned")
    t.shot("beam")
    t.relay("reset", settle=0.5)

    def check(r):
        r.note("catcher powered count: %s at ready, %s at the end (the player in the beam blocks it)" % (r.w(r.at("ready"), "caught"), r.w(r.last("ready", "burned"), "caught")))
        p = r.pos(r.at("on"))
        r.require(abs(p[0] - em[0]) < 24, "the player stood in the beam's line when it came on (%.0f u off)" % abs(p[0] - em[0]))
        n, dmg, src = r.damage("ready", "burned")
        r.expect(n >= 1, "hurt by the laser (%d hits, %.0f damage %s)" % (n, dmg, src))
        if r.mode == "mc":
            r.expect("onFire" in src, "as a burn (Minecraft source onFire): %s" % src)
    return check


@test("fling", title="walk off the 640 u ledge onto the floor: Minecraft takes fall damage, Chell doesn't (long fall boots)", order=85)
def fall(t):
    s = ent("fling", "tb_spot_fling_high")["origin"]
    t.relay("reset", settle=1.0)
    t.place([s[0], s[1], s[2]], 90)
    t.ready()
    t.hold("forward", 1.5)
    t.wait(1.5)
    t.event("landed")

    def check(r):
        r.require(r.drop("ready", "landed") > 500, "fell %.0f u" % r.drop("ready", "landed"))
        n, dmg, src = r.damage("ready", "landed")
        if r.mode == "mc":
            r.expect(n >= 1 and "fall" in src, "Minecraft took fall damage (%d hits, %.0f damage %s)" % (n, dmg, src))
        else:
            r.expect(n == 0, "Chell took no damage (%.0f)" % dmg)
    return check


@test("goo", title="walk into the goo: it kills", order=99, needs={"mc": "#32 damage sources"})
def deadly(t):
    t.relay("reset", settle=1.0)
    t.place(at_v("goo", 110), BAYS["goo"]["spot"]["yaw"])
    t.ready()
    t.hold("forward", 1.2)
    t.wait(2.0)
    t.event("in")
    t.lethal()

    def check(r):
        low = min(p[2] for p in r.path("ready", "in"))
        r.require(low < -16, "the player went into the pit (lowest %.0f u)" % low)
        n, dmg, src = r.damage("ready", "in")
        r.expect(n >= 1 and dmg >= 20, "hurt by the goo (%d hits, %.0f damage %s)" % (n, dmg, src))
        if r.mode == "mc":
            r.expect("lava" in src, "as goo (Minecraft source lava): %s" % src)
    return check


# ------------------------------------------------------------------------------------------------ the mode switch
@test("lift", modes=("chell",), title="J to the Minecraft player and back: same spot and facing", order=5)
def switch(t):
    o, yaw = BAYS["lift"]["spot"]["origin"], BAYS["lift"]["spot"]["yaw"]
    t.place(o, yaw)
    t.ready()
    t.cmd("bridge_mode mc")
    t.until("mcmode", 15)
    t.wait(1.0)
    t.event("as_mc")
    t.cmd("bridge_mode chell")
    t.until("chellmode", 15)
    t.wait(1.0)
    t.event("as_chell")

    def check(r):
        p0 = r.pos(r.at("ready"))
        f = r.last("ready", "as_mc")
        r.require(f["m"] == "mc", "switched to the Minecraft player")
        p1 = r.pos(f)
        r.expect(math.dist(p0, p1) < 16, "the Minecraft player is where Chell stood (%.1f u off)" % math.dist(p0, p1))
        r.expect(abs(((f["p2"]["a"][1] - yaw) + 540) % 360 - 180) < 3, "facing kept (yaw %.1f, was %d)" % (f["p2"]["a"][1], yaw))
        g = r.last("as_mc", "as_chell")
        r.require(g["m"] == "chell", "switched back to Chell")
        r.expect(math.dist(p1, r.pos(g)) < 16, "Chell is where the Minecraft player stood (%.1f u off)" % math.dist(p1, r.pos(g)))
    return check


# ------------------------------------------------------------------------------------------------ arrival (own run: the map's start)
@test("arrival", title="start of the map: the car arrives, the door opens, walk out into the hall", run="arrival", order=0)
def walk_out(t):
    t.watch("door", "prop_testchamber_door", "@entry_door", "m_bIsOpen", "b")
    t.watch_origin("car", "arrival_elevator-elevator_1")
    last = 0.0
    for at in (0.5, 1.0, 1.5, 2.0, 3.0, 4.0):  # what the screen shows during the ride (the user saw falls the trace missed)
        t.wait(at - last)
        last = at
        t.shot("ride_%04.1fs" % at)
    t.wait(14.0 - last)
    t.event("arrived")
    t.shot("arrived")
    t.look(0, BAYS["arrival"]["spot"]["yaw"] + 180)
    t.ready()
    t.hold("forward", 6.5)
    t.event("walked")
    t.shot("walked")

    def check(r):
        vs = [v_of("arrival", p) for p in r.path("ready", "walked")]
        r.require(vs[0] > 300, "the player started in the car, deep in the bay (%.0f u in)" % vs[0])
        ho = r.handover()
        r.expect(ho is not None and ho[0] < 0.5, "the Minecraft player leads from the start (took over after %s s; the user sees Chell until then)" % ("%.1f" % ho[0] if ho else "never"))
        r.expect(ho is not None and ho[1] < 8, "the handover doesn't move the player (jumped %s u on the switch frame)" % ("%.0f" % ho[1] if ho else "?"))
        # (the level's very first frame has the player at the origin, before it is put in the car: skip it)
        zs = [r.pos(f)[2] for f in r.span(None, "arrived") if max(abs(c) for c in r.pos(f)) > 1]
        top, worst = -1e9, 0
        for z in zs:  # the car only rises: any drop below the highest point so far is a fall through its floor
            top = max(top, z)
            worst = max(worst, top - z)
        cz = [r.w(f, "car")[2] for f in r.span(None, "arrived") if r.w(f, "car")]
        if cz:
            r.require(max(cz) - min(cz) > 200, "the car rose (%.0f u) as the level started" % (max(cz) - min(cz)))
            gap = max(r.w(f, "car")[2] - r.pos(f)[2] for f in r.span(None, "arrived") if r.w(f, "car"))
            r.note("car origin z %.0f..%.0f; the player was at most %.0f u below the car origin" % (min(cz), max(cz), gap))
        r.expect(worst < 40, "rode the car up without dropping through it (largest drop %.0f u)" % worst)
        falls = r.airborne_drops(None, "arrived")
        r.expect(not falls, "the Minecraft player never fell (%s)" % (", ".join("%.0f u at t %.1f s, %.0f u/s" % (d, t, v) for t, d, v in falls[:3]) or "no free fall"))
        opened = [f["t"] for f in r.span("ready", "walked") if r.w(f, "door") == 1]
        r.expect(opened, "the entrance door opened for the arriving player (m_bIsOpen)")
        r.expect(min(vs) < -48, "walked out into the hall (got to %.0f u from the bay opening; the hall is below 0)" % min(vs))
    return check


def _control(t, bay, label, name, kind):
    t.watch(label, "*", name, "m_angRotation" if kind == "v" else "-", kind)
    t.relay("reset")
    o, yaw = BAYS[bay]["spot"]["origin"], BAYS[bay]["spot"]["yaw"]
    t.place(o, yaw)
    t.ready()
    t.relay("go")
    t.event("moved")
    t.shot("moved")

    def check(r):
        a, b = r.w(r.at("ready"), label), r.w(r.last("ready", "moved"), label)
        r.require(a is not None, "%s exists" % name)
        d = max(abs((b[i] - a[i] + 540) % 360 - 180) for i in range(3)) if kind == "v" else math.dist(a, b)
        r.expect(d > 10, "with nobody on it, %s moved: %.0f %s" % (name, d, "deg" if kind == "v" else "u"))
    return check


@test("tilt", modes=("chell",), title="control: the tilting platform tilts with nobody on it", order=20)
def control(t):
    return _control(t, "tilt", "ang", "tb_tilt", "v")


@test("departure", modes=("chell",), title="control: the departure car leaves with nobody in it", order=79)
def control(t):  # noqa: F811
    return _control(t, "departure", "car", "departure_elevator-elevator_1", "o")


@test("cubes", title="look at a cube and use it: picked up", needs={"mc": "#32 right-click use"})
def pickup(t):
    c = ent("cubes", "tb_cube")["origin"]
    d = into("cubes")
    t.watch_origin("cube", "tb_cube")
    _cube(t)
    t.place([c[0] - d[0] * 72, c[1] - d[1] * 72, 8], BAYS["cubes"]["spot"]["yaw"])
    t.ready()
    t.face("tb_cube")
    t.wait(0.3)
    t.tap("use")
    t.wait(1.0)
    t.event("used")
    t.look(-10, BAYS["cubes"]["spot"]["yaw"])
    t.wait(1.0)
    t.event("lifted")
    t.shot("holding")
    t.tap("use")
    t.wait(0.5)

    def check(r):
        c0 = r.w(r.at("ready"), "cube")
        r.require(c0 is not None, "a cube is there")
        held = [f["p2"]["hold"] for f in r.span("used", "lifted")]
        r.expect(held and all(held), "holding the cube (%d of %d frames)" % (sum(held), len(held)))
        c1 = r.w(r.last("used", "lifted"), "cube")
        r.expect(c1[2] - c0[2] > 24, "the cube came up off the floor with the view (%.0f u)" % (c1[2] - c0[2]))
    return check


# ------------------------------------------------------------------------------------------------ digging (bridge #33; own run: -k dig)
def _dig_on(t):
    t.cmd("pc_dig 1", "pcb_dig_reset")  # the host's set and Minecraft's (/pcdig clear)
    t.wait(3.0)  # the harvest loads the level file (~30 ms) and re-exports around the player


def _cell_centre(p):
    """The centre of the 40 u dig cell column holding p (cells start at multiples of 40 in x and y)."""
    return [math.floor(p[0] / 40) * 40 + 20, math.floor(p[1] / 40) * 40 + 20, p[2]]


def _host_hole(t, bay):
    o = _cell_centre(BAYS[bay]["spot"]["origin"])
    _dig_on(t)
    t.place(o, 90)
    t.ready()
    t.cmd("pcb_dig_status", "pcb_dig_under 2 1")
    t.wait(1.0)
    if t.mode == "chell":
        t.tap("jump")  # her ground is re-traced over the hole
    t.wait(1.5)
    t.event("dug")
    t.shot("in_hole")
    if t.mode == "chell":
        t.cmd("pcb_trace_box %.1f %.1f 40 %.1f %.1f -80" % (o[0], o[1], o[0], o[1]))
    t.cmd("pcb_dig_status", "pcb_dig_under 2 0")
    t.wait(1.0)

    def check(r):
        r.note(" / ".join(l.split("dig: ", 1)[-1] for l in r.log("dig:")[:8]))
        r.require(r.log("cells dug under"), "pcb_dig_under dug the cells under the hull")
        d = r.drop("ready", "dug")
        r.expect(d >= 30, "dropped %.0f u into the two-cell hole (about 47 expected)" % d)
    return check


@test("dig", modes=("mc",), title="two cells dug under the Minecraft player's feet (host side): it drops into the hole", run="dig", order=10)
def host_hole(t):
    return _host_hole(t, "lift")


@test("dig", modes=("chell",), title="two cells dug under Chell's feet (host side): she sinks into the hole", run="dig", order=20)
def chell_sinks(t):
    return _host_hole(t, "mlinear")


@test("dig", modes=("mc",), title="dig straight down through the hall floor with a pickaxe: through the floor and on into revealed blocks", run="dig", order=25)
def floor_by_hand(t):
    o = _cell_centre(BAYS["train"]["spot"]["origin"])
    _dig_on(t)
    t.cmd("pcb_mc_cmd item replace entity @p weapon.mainhand with minecraft:diamond_pickaxe", "pcb_mc_cmd pcdig destruction true")
    t.place(o, 90)
    t.look(89, 90)
    t.wait(0.5)
    t.ready()
    t.hold("attack", 14.0)
    t.wait(1.0)
    t.event("mined")
    t.shot("down")
    t.look(0, 90)
    t.wait(0.3)
    t.shot("shaft_wall")
    t.cmd("pcb_dig_status")

    def check(r):
        r.expect(r.log("from Minecraft"), "Minecraft opened cells and told Portal 2")
        d = r.drop("ready", "mined")
        r.expect(d >= 100, "dug down %.0f u (through the floor and at least two blocks below it)" % d)
        n, dmg, src = r.damage("ready", "mined")
        r.expect(n == 0 or "fall" not in src, "no fall damage digging down (%s)" % src)
        r.note(" / ".join(l.split("dig: ", 1)[-1][:160] for l in r.log("inside Portal 2")[:2]))
    return check


@test("dig", modes=("mc",), title="tunnel west through the hall's outer wall with a pickaxe, out past the edge of the map", run="dig", order=30)
def into_wall(t):
    # the west hall wall: brush x -1120..-1090 (its hall face is -1090), at y 1260; behind it is Portal 2's solid void
    start = [-1070.0, 1260.0, 8.0]
    _dig_on(t)
    t.cmd("pcb_mc_cmd item replace entity @p weapon.mainhand with minecraft:diamond_pickaxe", "pcb_mc_cmd pcdig destruction true")
    t.place(start, 180)
    t.ready()
    for step in range(5):
        for pitch in (20, 55):  # the body-height cell, then the feet-height one
            t.look(pitch, 180)
            t.wait(0.2)
            t.hold("attack", 4.0)
        t.look(0, 180)
        t.hold("forward", 0.6)
        t.event("step%d" % step)
    t.event("tunnelled")
    t.shot("in_tunnel")
    t.cmd("pcb_dig_status")
    t.look(0, 0)  # turn round: the chamber back through the tunnel mouth
    t.wait(0.4)
    t.shot("looking_back")

    def check(r):
        xs = [p[0] for p in r.path("ready", "tunnelled")]
        import re
        dug = [int(m.group(1)) for l in r.log("dug cell", after="ready") for m in [re.search(r"(\d+) dug cells", l)] if m]
        r.expect(dug and max(dug) >= 4, "Minecraft opened cells in the wall and told Portal 2 (up to %s dug)" % (max(dug) if dug else 0))
        r.require(not r.log("EMBEDDED", after="ready"), "the player was never stuck inside the wall's solid after setup")
        r.expect(min(xs) < -1130, "tunnelled through the wall: reached x %.0f (the wall is x -1120..-1090; beyond it is the map's void)" % min(xs))
        n, dmg, src = r.damage("ready", "tunnelled")
        r.expect(n == 0, "not hurt in the tunnel (%d hits %s)" % (n, src))
        r.expect(not r.log("body was teleported"), "the body was never yanked back (%d teleport lines)" % len(r.log("body was teleported")))
        r.note(" / ".join(l.split("dig: ", 1)[-1][:160] for l in r.log("inside Portal 2")[:2]))
    return check


@test("tilt", title="stand on Valve's arm panel (tb_ramp), animate the arm: the player moves with the panel", needs={"chell": "map: tb_ramp isn't built like Valve's panels yet (Chell isn't carried)"})
def ramp(t):
    e = ent("tilt", "tb_ramp")
    c = e["center"]
    t.watch_origin("panel", "tb_ramp")
    t.relay("reset", settle=2.5)
    t.place([c[0], c[1], e["top"] + 2], (BAYS["tilt"]["spot"]["yaw"] + 180) % 360, fall=12)
    t.ready()
    t.relay("go", settle=2.5)
    t.event("moved")
    t.shot("after")

    def check(r):
        p0w, p1w = r.w(r.at("ready"), "panel"), r.w(r.last("ready", "moved"), "panel")
        r.require(p0w is not None, "tb_ramp exists")
        moved = math.dist(p0w, p1w)
        r.require(moved >= 10, "the panel itself moved %.0f u (about 23 along +v and 5 up expected)" % moved)
        p0, p1 = r.pos(r.at("ready")), r.pos(r.last("ready", "moved"))
        got = math.dist(p0, p1)
        r.expect(abs(got - moved) < 16 and r.drop("ready", "moved") < 24, "the player moved %.0f u with the panel (panel %.0f), never fell (dropped %.0f)" % (got, moved, r.drop("ready", "moved")))
    return check


@test("arrival", modes=("mc",), run="manual", title="(not a valid repro: after a console `map` reload Valve's car stays parked) restart the map 8 times as the Minecraft player (one Minecraft kept running); never fall through the arriving car", order=1)
def restarts(t):
    car = ent("arrival", "arrival_elevator")["origin"]
    t.wait(16.0)
    t.event("arrived0")
    for i in range(1, 9):
        t.cmd("map pc_testbed")
        t.wait(4.0)
        t.until("spawned", 60)
        t.wait(16.0)  # the car rises and stops
        t.event("arrived%d" % i)
        t.shot("arrived%d" % i)

    def check(r):
        fell = []
        for i in range(0, 9):
            f = r.last(None, "arrived%d" % i)
            p = r.pos(f)
            inside = math.hypot(p[0] - car[0], p[1] - car[1]) < 120
            ok = f["m"] == "mc" and p[2] > -40 and inside
            r.note("start %d: %s at (%.0f %.0f %.0f)%s" % (i, f["m"], p[0], p[1], p[2], "" if ok else "  <-- FELL / wrong"))
            if not ok:
                fell.append(i)
        guards = len(r.log("fall guard")) + len(r.log("hand-over"))
        r.note("fall-guard / hand-over BUG lines: %d" % guards)
        r.expect(not fell, "standing in the car after every start (%d of 9 failed: %s)" % (len(fell), fell))
    return check


# ------------------------------------------------------------------------------------------------ any map (own run: -k campaign --map <map> --start mc)
@test("campaign", modes=("mc",), run="campaign", title="any map that starts in Valve's arrival elevator: start as the Minecraft player, ride the car up without falling through it")
def arrival(t):
    t.watch_class("trains", "func_tracktrain")
    t.event("loaded")
    t.ready()
    last = 0.0
    for at in (0.5, 1.0, 1.5, 2.0, 3.0, 4.0, 6.0):
        t.wait(at - last)
        last = at
        t.shot("ride_%04.1fs" % at)
    t.wait(25.0 - last)
    t.event("arrived")
    t.shot("arrived")

    def check(r):
        fs = [f for f in r.span(None, "arrived") if max(abs(c) for c in r.pos(f)) > 1]  # from the level's first real frame
        r.require(fs, "the level ran")
        ho = r.handover()
        r.expect(ho is not None and ho[0] < 0.5, "the Minecraft player leads from the start (took over after %s s; the user sees Chell until then)" % ("%.1f" % ho[0] if ho else "never"))
        r.expect(ho is not None and ho[1] < 8, "the handover doesn't move the player (jumped %s u on the switch frame)" % ("%.0f" % ho[1] if ho else "?"))
        zs = [r.pos(f)[2] for f in fs]
        top, worst, at = -1e9, 0, None
        for f, z in zip(fs, zs):
            if z > top:
                top = z
            if top - z > worst:
                worst, at = top - z, f["t"]
        r.note("height %.0f .. %.0f over %.0f s; func_tracktrain count %s" % (min(zs), max(zs), fs[-1]["t"] - fs[0]["t"], r.w(fs[0], "trains")))
        r.note("the body moved %.0f u up / %.0f u down with the car" % (max(zs) - zs[0], zs[0] - min(zs)))
        falls = r.airborne_drops(None, "arrived")
        r.expect(not falls, "the Minecraft player never fell (%s)" % (", ".join("%.0f u at t %.1f s, %.0f u/s" % (d, t, v) for t, d, v in falls[:3]) or "no free fall"))
        mcs = [f for f in fs[-30:] if "mc" in f]
        r.expect(mcs and mcs[-1]["mc"]["g"] == 1, "standing on something at the end (Minecraft ground flag %s)" % (mcs[-1]["mc"]["g"] if mcs else None))
        r.note("BUG lines: %d (fall guard %d)" % (len(r.log("BUG")), len(r.log("fall guard"))))
    return check


@test("campaign", modes=("mc",), run="campaign_linked", title="the user's case in any chamber: Minecraft already leads, then the chamber loads; the Minecraft player is in the arrival car from frame 1 and must never fall through it while it moves")
def linked(t):
    import core
    target = core.TARGET_MAP or "sp_a2_laser_intro"
    t.cmd("bridge_mode mc")
    t.until("mcmode", 60)
    t.until("mcleads", 30)
    t.wait(3.0)
    t.event("before_load")
    t.cmd("map %s" % target)
    t.wait(1.0)
    t.until("map%s" % target, 120)
    t.event("loaded")
    t.ready()
    last = 0.0
    for at in (0.5, 1.0, 1.5, 2.0, 3.0, 4.0, 6.0):
        t.wait(at - last)
        last = at
        t.shot("ride_%04.1fs" % at)
    t.wait(25.0 - last)
    t.event("arrived")
    t.shot("arrived")

    def check(r):
        fs = [f for f in r.span("loaded", "arrived") if max(abs(c) for c in r.pos(f)) > 1]
        r.require(fs, "the chamber %s loaded and ran" % target)
        r.require(r.log("level init %s" % target), "the level change to %s happened" % target)
        modes = [f["m"] for f in fs[:30]]
        r.expect(all(m == "mc" for m in modes), "the Minecraft player leads from the chamber's first frames (%d of the first %d frames)" % (modes.count("mc"), len(modes)))
        zs = [r.pos(f)[2] for f in fs]
        r.require(max(zs) - min(zs) > 100, "the arrival car moved the player (%.0f u up / %.0f u down)" % (max(zs) - zs[0], zs[0] - min(zs)))
        r.note("ride: body z %.0f -> %.0f (range %.0f..%.0f)" % (zs[0], zs[-1], min(zs), max(zs)))
        falls = r.airborne_drops("loaded", "arrived")
        r.expect(not falls, "never fell through the moving car (%s)" % (", ".join("%.0f u at t %.1f s, %.0f u/s" % (d, tt, v) for tt, d, v in falls[:3]) or "no free fall"))
        mcs = [f for f in fs[-30:] if "mc" in f]
        r.expect(mcs and mcs[-1]["mc"]["g"] == 1, "standing at the end")
        r.note("guard/BUG lines: %d" % (len(r.log("fall guard")) + len(r.log("BUG"))))
    return check


# bridge #45: portals placed by Chell's own gun in a campaign chamber (the mirror world has no Minecraft region), then the Minecraft player
# walks into one (J). Two facing walls of one corridor per map: (x, y, z of Chell's feet, yaw to face a wall; the wall of colour 0 first).
CAMPAIGN_PAIRS = {
    "sp_a2_bridge_the_gap": {"stand": [-495.0, -640.0, 1150.0], "blue_yaw": 90, "orange_yaw": 270},
    "sp_a2_column_blocker": {"stand": [-700.0, 256.0, 1.0], "blue_yaw": 270, "orange_yaw": 90, "aim": True},  # (bridge #49: only the stage tests use this one)
}


@test("campaign", modes=("mc",), run="campaign_portals", title="gun portals in a campaign chamber (mirror world): shoot a pair as Chell, switch to the Minecraft player, walk into one portal and come out of the other still walking")
def campaign_portals(t):
    import core
    mp = core.TARGET_MAP or "sp_a2_bridge_the_gap"
    cp = CAMPAIGN_PAIRS[mp]
    st = cp["stand"]
    t.event("loaded")
    t.cmd("bridge_mode chell")
    t.until("chellmode", 30)
    t.wait(1.0)
    t.cmd("setpos %.1f %.1f %.1f" % tuple(st), "setang 0 %d 0" % cp["blue_yaw"])
    t.wait(1.0)
    t.cmd("+attack")
    t.wait(0.15)
    t.cmd("-attack")
    t.wait(2.0)
    t.cmd("setang 0 %d 0" % cp["orange_yaw"])
    t.wait(0.5)
    t.cmd("+attack2")
    t.wait(0.15)
    t.cmd("-attack2")
    t.wait(2.0)
    t.event("shot")
    # what Chell's own view shows with the pair placed (no extra outlines, no stray rims): from the corridor's far end and from close to a portal
    t.cmd("setpos -800.0 -640.0 1150.0", "setang 0 0 0")
    t.wait(1.5)
    t.shot("chell_far")
    t.cmd("setpos -600.0 -640.0 1150.0", "setang 0 -30 0")
    t.wait(1.5)
    t.shot("chell_near")
    t.cmd("bridge_mode mc")
    t.until("mcmode", 30)
    t.wait(1.0)
    t.place(st, cp["orange_yaw"])
    t.wait(1.0)
    t.ready()
    t.shot("before")
    t.hold("forward", 2.2)
    t.wait(0.5)
    t.event("walked")
    t.shot("after")

    def check(r):
        f0 = r.at("ready")
        ps = r.portals(f0)
        r.require(sum(1 for p in ps if p[1] == 1) >= 2, "both gun portals open at ready (%s)" % ps)
        r.mode_ok("ready")
        js = r.jumps("ready", "walked", min_u=64)
        r.expect(js, "a portal transit happened (a jump in position); start %s end %s" % (r.pos(f0), r.pos(r.last("ready", "walked"))))
        if js:
            tj, p_in, p_out = js[0]
            r.note("transit at t %.2f: from (%.0f %.0f %.0f) to (%.0f %.0f %.0f)" % ((tj,) + tuple(p_in) + tuple(p_out)))
            r.expect(r.speed_h(None, None) > 60, "kept moving after the transit (%.0f u/s)" % r.speed_h(None, None))
        r.note("transit lines in the game's log: %d" % len(r.log("player transit")))
    return check


# bridge #48: arrows (a bow, drawn and loosed with the player's own inputs) meet a campaign chamber's geometry like blocks: they stick in its wall and
# floor, stick in a placed block, go through an open portal and stick on the far side, and are still there seconds later. The mod logs every arrow's
# hit and its persistence in mc.log ("Portalcraft: arrow #N hit <host geometry|block ...|mover N> at ...", "... still stuck after 100 ticks");
# the test marks each section with `pcb_mc_cmd tag @p add tb_<name>` and the check reads the lines between the markers.
@test("campaign", modes=("mc",), run="campaign_arrows", title="arrows in a campaign chamber (mirror world): stick in the wall, the floor and a placed block, fly through a portal; none vanish")
def campaign_arrows(t):
    import core
    mp = core.TARGET_MAP or "sp_a2_bridge_the_gap"
    cp = CAMPAIGN_PAIRS[mp]
    st = cp["stand"]
    t.event("loaded")
    t.cmd("bridge_mode mc")
    t.until("mcmode", 30)
    t.wait(1.0)
    t.place(st, cp["orange_yaw"])
    t.wait(1.0)
    t.ready()
    t.cmd("pcb_mc_cmd item replace entity @p weapon.mainhand with minecraft:bow", "pcb_mc_cmd give @p minecraft:arrow 40")
    t.wait(1.0)

    def shoot(name, pitch, yaw):
        t.cmd("pcb_mc_cmd tag @p add tb_%s" % name)
        t.wait(0.8)
        t.look(pitch, yaw)
        t.wait(0.4)
        t.hold("use", 1.3)
        t.wait(6.5)  # an arrow is checked 100 ticks (5 s) after it stuck
        t.event(name)

    shoot("wall", 0, cp["orange_yaw"])
    shoot("floor", 35, cp["orange_yaw"])
    t.cmd("pcb_mc_cmd execute as @p at @p anchored eyes positioned ^ ^ ^3 run setblock ~ ~ ~ minecraft:stone")
    shoot("block", 0, cp["orange_yaw"])
    t.cmd("pcb_mc_cmd execute as @p at @p anchored eyes positioned ^ ^ ^3 run setblock ~ ~ ~ minecraft:air")
    # the gun's pair (as in campaign_portals), then an arrow at the orange portal
    t.cmd("bridge_mode chell")
    t.until("chellmode", 30)
    t.wait(1.0)
    t.cmd("setpos %.1f %.1f %.1f" % tuple(st), "setang 0 %d 0" % cp["blue_yaw"])
    t.wait(1.0)
    t.cmd("+attack")
    t.wait(0.15)
    t.cmd("-attack")
    t.wait(2.0)
    t.cmd("setang 0 %d 0" % cp["orange_yaw"])
    t.wait(0.5)
    t.cmd("+attack2")
    t.wait(0.15)
    t.cmd("-attack2")
    t.wait(2.0)
    t.cmd("bridge_mode mc")
    t.until("mcmode", 30)
    t.wait(1.0)
    t.place(st, cp["orange_yaw"])
    t.wait(1.0)
    t.cmd("pcb_mc_cmd item replace entity @p weapon.mainhand with minecraft:bow")
    shoot("portal", 0, cp["orange_yaw"])
    t.shot("end")

    def check(r):
        import re
        r.mode_ok("ready")
        order = ["wall", "floor", "block", "portal"]
        for i, name in enumerate(order):
            nxt = "tb_" + order[i + 1] if i + 1 < len(order) else None
            lines = r.mclog("arrow #", after="tb_" + name, before=nxt)
            hits = [re.search(r"arrow #(\d+) hit (.+?) at \(?([-\d., ]+)", l) for l in lines if " hit " in l]
            hits = [(m.group(1), m.group(2)) for m in hits if m]
            stuck = {m.group(1) for m in (re.search(r"arrow #(\d+) still stuck", l) for l in lines) if m}
            fell = [l for l in lines if "falls" in l]
            r.note("%s: %s; still stuck after 100 ticks: %s" % (name, ", ".join("#%s %s" % h for h in hits) or "no arrow hit anything", sorted(stuck) or "none"))
            want = "block minecraft:stone" if name == "block" else "host geometry"
            mine = [h for h in hits if h[1].startswith(want)]
            r.expect(mine, "%s: an arrow stuck in %s (hits: %s)" % (name, "the placed stone block" if name == "block" else "the chamber's geometry", hits or "none"))
            r.expect(any(h[0] in stuck for h in mine), "%s: that arrow was still in place 5 s later (stuck: %s)" % (name, sorted(stuck) or "none"))
            r.expect(not fell, "%s: nothing made it fall again (%s)" % (name, fell[:1]))
        through = r.mclog("arrow went through portal", after="tb_portal")
        r.expect(through, "portal: an arrow went through the open portal (%d such log lines)" % len(through))
    return check


# bridge #48: an arrow that sticks in a mover is carried with it (pc_testbed lift: a 160 x 160 u platform that rises 192 u in 3 s). The arrow is
# loosed down at the platform while it is low, the lift is started, and 5 s after it stuck the mod logs where the arrow is.
@test("lift", modes=("mc",), run="arrows", title="loose an arrow at the lift platform, start the lift: the arrow is stuck in the platform and rises with it")
def arrow_in_lift(t):
    e = ent("lift", "tb_lift")
    o, yaw = inside("lift")
    t.watch_origin("mover", "tb_lift")
    t.relay("reset", settle=BAYS["lift"]["settle_s"])
    t.place(o, yaw)
    t.ready()
    t.cmd("pcb_mc_cmd item replace entity @p weapon.mainhand with minecraft:bow", "pcb_mc_cmd give @p minecraft:arrow 8")
    t.wait(1.0)
    t.cmd("pcb_mc_cmd tag @p add tb_lift_arrow")
    t.wait(0.8)
    t.look(12, yaw)
    t.wait(0.4)
    t.hold("use", 1.3)
    t.relay("go", settle=1.0)
    t.wait(6.0)
    t.event("moved")
    t.shot("end")

    def check(r):
        import re
        m0, m1 = r.w(r.at("ready"), "mover"), r.w(r.last("ready", "moved"), "mover")
        r.require(m0 is not None and m1[2] - m0[2] >= 150, "the lift itself rose %.0f u" % ((m1[2] - m0[2]) if m0 and m1 else 0))
        lines = r.mclog("arrow #", after="tb_lift_arrow")
        hit = next((re.search(r"arrow #(\d+) hit (.+?) at \(([-\d.]+), ([-\d.]+), ([-\d.]+)\)", l) for l in lines if " hit " in l), None)
        r.expect(hit, "an arrow hit something (%s)" % ("; ".join(l.split("Portalcraft: ")[-1] for l in lines[:3]) or "no arrow line in the log"))
        if not hit:
            return
        r.expect(hit.group(2).startswith("mover"), "it hit the lift platform as a mover (%s)" % hit.group(2))
        still = next((re.search(r"arrow #%s still stuck after 100 ticks at \(([-\d.]+), ([-\d.]+), ([-\d.]+)\)" % hit.group(1), l) for l in lines), None)
        r.expect(still, "the arrow was still there 5 s later")
        if still:
            rise = float(still.group(2)) - float(hit.group(4))
            r.expect(rise > 3.5, "the arrow rose %.2f blocks with the platform (the lift rose %.1f)" % (rise, (m1[2] - m0[2]) / 40.0))
    return check


# bridge #49: the Minecraft portal (G) in a campaign chamber. Chell holds G while firing at the nearest white wall (pcb_aim_portalable turns to it, so no
# wall is typed per map); the chamber has no hidden Minecraft box, so the plugin makes one (a virtual region in an empty box of Portal 2's world, shown on
# the Minecraft "stage" the mod generates high above the chambers: stage.cpp, StageTerrain.java), the partner opens on the stage's cliff, and the player walks
# through and back. Only the stand spot is per map (test data; the feature needs nothing).
def _shot(t, label):
    """Portal 2's own screenshot of the composited frame (named pcb_*: run_game.sh moves it to cache/bridge/shots/tb_<run>/)."""
    t.cmd("screenshot pcb_s49_%s_%s" % (t.mode, label))
    t.wait(0.7)


def _stage_fire(t, mp):
    """One deliberate setup: Chell on a proven spot facing a wall that takes a portal (the same as campaign_portals), G held while she fires; wait for the pair
    (the exit found and the partner moved there, or the run ends: nothing after it can mean anything without it)."""
    cp = CAMPAIGN_PAIRS[mp]
    t.cmd("pcb_assert_soft 1")
    t.until("spawned", 60)
    t.until("link", 150)  # (Minecraft is up and linked: the stage needs it)
    t.wait(20.0 if cp.get("aim") else 3.0)  # (a spot picked by the aim helper is in the chamber, not the arrival elevator: wait for the car to open)
    t.event("loaded")
    t.cmd("bridge_mode chell")
    t.until("chellmode", 30)
    t.cmd("pcb_noclip_set 0", "setpos %.1f %.1f %.1f" % tuple(cp["stand"]), "setang 0 %d 0" % cp["blue_yaw"])
    if cp.get("aim"):
        t.cmd("pcb_aim_portalable")  # (a spot without a proven wall: the nearest flat white one)
    t.wait(1.0)
    t.event("aimed")
    t.cmd("pcb_key_hold arm 25")  # the G key as held through the same polling as the real one: it arms, the stage starts
    t.cmd("waituntil haveexit 1200 abort")  # (the stage's cliff found in the streamed blocks)
    t.cmd("+attack")
    t.wait(0.15)
    t.cmd("-attack")
    t.cmd("waituntil gpair 600 abort")  # (both portals open, one in Minecraft)
    t.wait(1.0)
    t.event("shot")


def _stage_checks(r, walked_ev="walked", home_ev="home"):
    f = r.at("shot")
    ps = [p for p in r.portals(f) if p[1] == 1]
    r.require(len(ps) >= 2, "both portals of the G shot are open (%s)" % [(p[0], [round(c) for c in p[2]]) for p in r.portals(f)])
    far = math.dist(ps[0][2], ps[1][2])
    r.expect(far > 3000, "the partner is far from the chamber portal: %.0f u (the virtual region is an empty box of Portal 2's world)" % far)
    on = r.log("[bridge] stage: ON")
    r.expect(on, "the stage turned on for this level (%s)" % (on[0].split("stage: ")[-1][:150] if on else "no log line"))
    r.expect(not r.log("no portal onto Minecraft in this map"), "the map has room for the virtual region")
    js = r.jumps("ready", walked_ev, min_u=2000)
    r.expect(js, "walking into the chamber portal carried the player to the stage (a jump over 2000 u)")
    if js:
        r.note("to the stage: from (%.0f %.0f %.0f) to (%.0f %.0f %.0f)" % (tuple(js[0][1]) + tuple(js[0][2])))
    back = r.jumps(walked_ev, home_ev, min_u=2000)
    r.expect(back, "walking into the cliff portal carried the player back to the chamber")
    if back:
        r.note("home: from (%.0f %.0f %.0f) to (%.0f %.0f %.0f)" % (tuple(back[0][1]) + tuple(back[0][2])))
        ds = [math.dist(q[2], back[0][2]) for q in r.portals(r.at(home_ev))]
        d = min(ds) if ds and min(ds) <= 400 else None
        r.expect(d is not None, "it came out at the chamber portal (%s u away)" % ("%.0f" % d if d is not None else "no portal within 400 u"))


def _walk_in(t):
    """Walk forward with the player's own input until the portal carries it (a transit counted), then stop."""
    t.cmd("pcb_assert_transits reset")
    t.key("forward", True)
    t.cmd("waituntil transit 480 abort")
    t.key("forward", False)
    t.wait(1.0)


@test("campaign", modes=("chell",), run="campaign_stage", title="G in a campaign chamber (mirror world): Chell fires a portal with G held, its partner opens on the Minecraft stage, she walks through and back")
def campaign_stage_chell(t):
    import core
    mp = core.TARGET_MAP or "sp_a2_bridge_the_gap"
    _stage_fire(t, mp)
    t.cmd("pcb_face_portal 0", "pcb_stop")  # (she stands where she fired from: the portal is a few steps ahead)
    t.wait(1.0)
    _shot(t, "chamber_portal")
    t.ready()
    _walk_in(t)
    t.event("walked")
    for k, w in enumerate((3.0, 8.0, 12.0)):  # (is the stage drawn at all, and how soon: Minecraft's own image of the next view, cache/bridge/mcview)
        t.wait(w)
        t.cmd("pcb_mcshot")
        t.wait(1.0)
        _shot(t, "stage_arrival%d" % k)
    t.cmd("pcb_face_portal 1", "pcb_stop")  # (the transit put her 140 u out from the cliff portal, facing away)
    t.wait(1.0)
    _shot(t, "cliff_portal")
    t.hold("forward", 2.5)  # (the portal is a few steps ahead: she stopped near it)
    t.wait(1.5)
    t.event("home")
    _shot(t, "chamber_again")

    def check(r):
        r.mode_ok("ready")
        _stage_checks(r)
    return check


@test("campaign", modes=("chell",), run="campaign_stage", title="(bridge #74) a pair inside the Minecraft stage: Chell fires a second portal onto the stage's floor, both portals show their views (Minecraft to Minecraft)")
def campaign_stage_pair(t):
    import core
    mp = core.TARGET_MAP or "sp_a2_bridge_the_gap"
    _stage_fire(t, mp)
    t.cmd("pcb_face_portal 0", "pcb_stop")
    t.wait(1.0)
    t.ready()
    _walk_in(t)
    t.event("walked")
    t.wait(8.0)
    t.cmd("pcb_key_hold arm 0")  # (the G key's 25 s hold of the first shot is over: this shot is a plain one)
    t.cmd("pcb_face_floor 1 150 70", "pcb_stop")  # (a floor spot between her and the cliff portal)
    t.wait(1.0)
    t.cmd("+attack")
    t.wait(0.15)
    t.cmd("-attack")
    t.wait(3.0)
    t.event("paired")
    t.cmd("pcb_face_portal 0")  # (the new portal on the stage floor)
    t.wait(2.5)
    _shot(t, "pair_floor")
    t.cmd("pcb_face_portal 1")  # (the cliff portal, now the other end of a pair that is wholly inside Minecraft)
    t.wait(2.5)
    _shot(t, "pair_cliff")

    def check(r):
        r.mode_ok("ready")
        ps = [p for p in r.portals(r.at("paired")) if p[1] == 1]
        r.require(len(ps) >= 2, "two portals are open (%s)" % [(p[0], [round(c) for c in p[2]]) for p in r.portals(r.at("paired"))])
        d = math.dist(ps[0][2], ps[1][2])
        r.expect(d < 1500, "both portals of the pair are on the stage (%.0f u apart)" % d)
    return check


@test("campaign", modes=("chell",), run="campaign_stage", title="(bridge #74) a Portal 2 cube standing on the Minecraft stage is drawn, depth-correct")
def campaign_stage_cube(t):
    import core
    mp = core.TARGET_MAP or "sp_a2_bridge_the_gap"
    _stage_fire(t, mp)
    t.cmd("pcb_face_portal 0", "pcb_stop")
    t.wait(1.0)
    t.ready()
    _walk_in(t)
    t.event("walked")
    t.wait(6.0)
    t.cmd("pcb_spawn_cube_before 1 90")
    t.wait(4.0)
    t.cmd("pcb_face_portal 1", "pcb_stop")
    t.wait(2.0)
    t.cmd("pcb_stage_props 1 14")
    t.wait(0.3)
    _shot(t, "cube_stage")
    t.cmd("pcb_face_floor 1 90 20")
    t.wait(1.5)
    t.cmd("pcb_model_light_dump 14409 -13100 -13682 70")
    t.wait(0.5)
    _shot(t, "cube_stage2")
    t.cmd("+use")  # (she looks at the cube from 60 u: picks it up)
    t.wait(0.3)
    t.cmd("-use")
    t.wait(1.5)
    t.cmd("setang 5 180 0")
    t.wait(1.5)
    _shot(t, "cube_held")
    t.cmd("setpos 14290 -13100 -13700", "setang 8 0 0", "pcb_stop")  # (the cube now stands between her and the cliff portal: a portal view is drawn around it)
    t.wait(2.0)
    _shot(t, "cube_before_portal")
    cp = CAMPAIGN_PAIRS[mp]  # (home by a teleport: the view from the chamber into the stage, the cube standing in front of the cliff portal)
    t.cmd("setpos %.1f %.1f %.1f" % tuple(cp["stand"]), "setang 0 %d 0" % cp["blue_yaw"], "pcb_stop")
    t.wait(1.5)
    t.cmd("pcb_face_portal 0")
    t.wait(4.0)
    _shot(t, "cube_from_chamber")

    def check(r):
        r.mode_ok("ready")
    return check


def _mc_forward(t, sec):
    """The Minecraft player's own W key (this test is registered as Chell's, which switches to the Minecraft player itself)."""
    t.cmd("pcb_mckey 26 1")
    t.wait(sec)
    t.cmd("pcb_mckey 26 0")


@test("campaign", modes=("chell",), run="campaign_stage", title="G in a campaign chamber (mirror world): the Minecraft player walks through Chell's G portal onto the Minecraft stage and back")
def campaign_stage_mcplayer(t):
    import core
    mp = core.TARGET_MAP or "sp_a2_bridge_the_gap"
    _stage_fire(t, mp)  # (the gun is Chell's: the Minecraft player has none; it uses the pair she made)
    t.cmd("bridge_mode mc")
    t.until("mcmode", 30)
    t.wait(1.0)
    t.cmd("pcb_mc_place_portal 0 64")
    t.cmd("waituntil mcplaced 900 abort")
    t.wait(1.0)
    t.cmd("pcb_face_portal 0")
    t.wait(1.0)
    _shot(t, "chamber_portal")
    t.ready()
    _mc_forward(t, 2.5)
    t.wait(2.0)
    t.event("walked")
    t.wait(4.0)
    _shot(t, "stage_arrival")
    t.cmd("pcb_face_portal 1")  # (it came out facing away from the cliff portal)
    t.wait(1.0)
    _shot(t, "cliff_portal")
    _mc_forward(t, 3.0)
    t.wait(1.5)
    t.event("home")
    _shot(t, "chamber_again")

    def check(r):
        r.require(r.at("ready")["m"] == "mc", "the Minecraft player leads at 'ready' (is %s)" % r.at("ready")["m"])
        _stage_checks(r)
    return check
