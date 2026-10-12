package dev.skycraft.world;

import static java.lang.foreign.ValueLayout.*;

import dev.skycraft.SkyCraft;
import dev.skycraft.link.Proto;
import dev.skycraft.link.SkyLink;
import java.lang.foreign.MemorySegment;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.WeakHashMap;
import java.util.concurrent.ConcurrentHashMap;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.phys.AABB;
import net.minecraft.world.phys.Vec3;

/**
 * Portalcraft (bridge #26): the host's movers. Every Portal 2 solid that can move or animate (doors, lifts, elevators,
 * arm panels, buttons, cubes) is a mover: a shape (boxes in its own frame, MC blocks; kColMoverShape) bound to a mover id
 * (kColMoverBind), and a pose per host frame (the McMovers block). Entities collide with each mover in its own frame,
 * exactly (an oriented box against the shape's boxes), are carried by the one they stand on, and pushed by one that
 * moves into them. Nothing here knows any particular mover.
 *
 * <p>The collision follows Create's ContraptionCollider (adapted from Create (MIT), Copyright (c) The Create Team /
 * The Creators of Create; ContraptionCollider.collideEntities, ContinuousOBBCollider, OrientedBB, see LICENSE-Create):
 * the entity's box is taken into the mover's local frame, where it is an oriented box against the mover's axis-aligned
 * boxes; a separating-axis test on the 6 face axes gives either the overlap (pushed out along the least penetration, or
 * lifted onto a top within step height) or, for the motion, the time of impact (a swept test: a fast mover or a fast
 * entity does not tunnel); responses go back to world space; a surface contact sets onGround, clears the fall distance
 * and carries the entity by the contact point's motion (translation and rotation). Differences from Create: the pass runs
 * at the head of the entity's own move (the platform's motion before the character's step), the carry uses the mover's
 * pose at the entity's last contact (the host's frame clock and Minecraft's ticks are not aligned), and a mover's
 * motion into an entity is swept from its previous pose (Create's client "save from clipping" ray does that job there).
 */
public final class SkyMovers {
	private SkyMovers() {
	}

	/** A shape: boxes in the mover's frame (MC blocks) and their bounds. */
	public record Shape(int id, AABB[] boxes, AABB bounds) {
	}

	private static final ConcurrentHashMap<Integer, Shape> SHAPES = new ConcurrentHashMap<>();
	private static final ConcurrentHashMap<Integer, Integer> BINDS = new ConcurrentHashMap<>();
	/** (bridge #43) A shape's dig lattice: {flags, anchor x, y, z} (COL_MOVER_LATTICE). */
	private static final ConcurrentHashMap<Integer, float[]> LATTICE = new ConcurrentHashMap<>();

	/** A rigid pose: world = (x, y, z) + R local, R row-major. */
	public static final class Pose {
		final double x, y, z;
		final double[] r;

		Pose(double x, double y, double z, double[] r) {
			this.x = x;
			this.y = y;
			this.z = z;
			this.r = r;
		}

		static Pose of(double x, double y, double z, double qx, double qy, double qz, double qw) {
			double n = Math.sqrt(qx * qx + qy * qy + qz * qz + qw * qw);
			if (n < 1e-9) {
				qw = 1;
				n = 1;
			}
			qx /= n;
			qy /= n;
			qz /= n;
			qw /= n;
			return new Pose(x, y, z, new double[] {
				1 - 2 * (qy * qy + qz * qz), 2 * (qx * qy - qz * qw), 2 * (qx * qz + qy * qw),
				2 * (qx * qy + qz * qw), 1 - 2 * (qx * qx + qz * qz), 2 * (qy * qz - qx * qw),
				2 * (qx * qz - qy * qw), 2 * (qy * qz + qx * qw), 1 - 2 * (qx * qx + qy * qy)});
		}

		Vec3 toWorld(double lx, double ly, double lz) {
			return new Vec3(x + r[0] * lx + r[1] * ly + r[2] * lz, y + r[3] * lx + r[4] * ly + r[5] * lz, z + r[6] * lx + r[7] * ly + r[8] * lz);
		}

		Vec3 toLocal(Vec3 p) {
			double dx = p.x - x, dy = p.y - y, dz = p.z - z;
			return new Vec3(r[0] * dx + r[3] * dy + r[6] * dz, r[1] * dx + r[4] * dy + r[7] * dz, r[2] * dx + r[5] * dy + r[8] * dz);
		}

		Vec3 dirToLocal(Vec3 v) {
			return new Vec3(r[0] * v.x + r[3] * v.y + r[6] * v.z, r[1] * v.x + r[4] * v.y + r[7] * v.z, r[2] * v.x + r[5] * v.y + r[8] * v.z);
		}

		Vec3 dirToWorld(double lx, double ly, double lz) {
			return new Vec3(r[0] * lx + r[1] * ly + r[2] * lz, r[3] * lx + r[4] * ly + r[5] * lz, r[6] * lx + r[7] * ly + r[8] * lz);
		}

		boolean same(Pose o) {
			if (o == null || Math.abs(o.x - x) + Math.abs(o.y - y) + Math.abs(o.z - z) > 1e-7) {
				return false;
			}
			for (int i = 0; i < 9; i++) {
				if (Math.abs(o.r[i] - r[i]) > 1e-7) {
					return false;
				}
			}
			return true;
		}

		/** The world bounds of a local box at this pose. */
		AABB place(AABB b) {
			double cx = (b.minX + b.maxX) * 0.5, cy = (b.minY + b.maxY) * 0.5, cz = (b.minZ + b.maxZ) * 0.5;
			double hx = (b.maxX - b.minX) * 0.5, hy = (b.maxY - b.minY) * 0.5, hz = (b.maxZ - b.minZ) * 0.5;
			Vec3 c = toWorld(cx, cy, cz);
			double ex = Math.abs(r[0]) * hx + Math.abs(r[1]) * hy + Math.abs(r[2]) * hz;
			double ey = Math.abs(r[3]) * hx + Math.abs(r[4]) * hy + Math.abs(r[5]) * hz;
			double ez = Math.abs(r[6]) * hx + Math.abs(r[7]) * hy + Math.abs(r[8]) * hz;
			return new AABB(c.x - ex, c.y - ey, c.z - ez, c.x + ex, c.y + ey, c.z + ez);
		}
	}

	/** One host frame's movers (immutable once published). */
	private static final class Snap {
		final int n;
		final int[] ids;
		final Pose[] cur, prev;
		final Shape[] shape;
		final AABB[] bounds;  // world bounds at cur
		final Map<Integer, Integer> index;

		Snap(int n) {
			this.n = n;
			ids = new int[n];
			cur = new Pose[n];
			prev = new Pose[n];
			shape = new Shape[n];
			bounds = new AABB[n];
			index = new HashMap<>(n * 2);
		}

		int find(int id) {
			Integer i = index.get(id);
			return i == null ? -1 : i;
		}
	}

	private static volatile Snap snap = new Snap(0);
	private static final int[] READ_IDS = new int[Proto.MAX_MOVERS];
	private static final double[] READ_POSE = new double[7 * Proto.MAX_MOVERS];
	private static final int[] READ_EPOCH = new int[1];

	public static void clear() {
		SHAPES.clear();
		BINDS.clear();
		LATTICE.clear();
		snap = new Snap(0);
	}

	// ---- collision ring (the collision thread) ----

	/** COL_MOVER_SHAPE: ColMoverShape {shapeId, count} + count boxes (6 floats). */
	public static void readShape(MemorySegment s, long p) {
		int id = s.get(JAVA_INT, p);
		int count = Math.min(s.get(JAVA_INT, p + 4), 200000);
		AABB[] boxes = new AABB[Math.max(count, 0)];
		AABB bounds = null;
		long e = p + Proto.COL_MOVER_SHAPE_BYTES;
		for (int i = 0; i < boxes.length; i++, e += Proto.COL_MOVER_BOX_BYTES) {
			AABB b = new AABB(s.get(JAVA_FLOAT, e), s.get(JAVA_FLOAT, e + 4), s.get(JAVA_FLOAT, e + 8), s.get(JAVA_FLOAT, e + 12), s.get(JAVA_FLOAT, e + 16), s.get(JAVA_FLOAT, e + 20));
			boxes[i] = b;
			bounds = bounds == null ? b : bounds.minmax(b);
		}
		if (bounds != null) {
			SHAPES.put(id, new Shape(id, boxes, bounds));
		}
		if (shapeLogs++ < 60) {
			SkyCraft.LOG.info("SkyCraft: mover shape {}: {} boxes, bounds {}", id, boxes.length, bounds);
		}
	}

	private static int shapeLogs;

	/** COL_MOVER_LATTICE (bridge #43): ColMoverLattice {shapeId, flags, ax, ay, az, pad}: the shape's dig lattice. */
	public static void readLattice(MemorySegment s, long p) {
		LATTICE.put(s.get(JAVA_INT, p), new float[] {s.get(JAVA_INT, p + 4), s.get(JAVA_FLOAT, p + 8), s.get(JAVA_FLOAT, p + 12), s.get(JAVA_FLOAT, p + 16)});
	}

	/** COL_MOVER_BIND: ColMoverBind {moverId, shapeId (0 = unbind)}. */
	public static void readBind(MemorySegment s, long p) {
		int mover = s.get(JAVA_INT, p);
		int shape = s.get(JAVA_INT, p + 4);
		if (shape == 0) {
			BINDS.remove(mover);
		} else {
			BINDS.put(mover, shape);
		}
	}

	// ---- poses (the client thread, once per client tick) ----

	/** The host's latest poses become the current ones; the last current ones become the previous ones. */
	public static void refresh() {
		long t0 = System.nanoTime();
		refreshInner();
		nanos += System.nanoTime() - t0;
		ticks++;
		long now = System.currentTimeMillis();
		if (now - lastReport > 20000) {
			if (lastReport != 0 && ticks > 0) {
				SkyCraft.LOG.info("SkyCraft: movers cost {} ms per client tick ({} ticks; {} movers posed now, {} shapes, {} binds; {} carries, {} pushes, {} sweep hits)",
					String.format("%.3f", nanos / 1e6 / ticks), ticks, snap.n, SHAPES.size(), BINDS.size(), carries, pushes, sweeps);
				SkyCraft.LOG.info("SkyCraft: movers ground snaps {}", snaps);
			}
			lastReport = now;
			nanos = 0;
			ticks = 0;
		}
	}

	private static long ticks, lastReport;

	/**
	 * (bridge #31) Movers the host posed whose shape or bind has not arrived through the collision ring yet (the ring is read on its own
	 * thread, a frame or two behind the poses). A player held for the host's collision stays held while this is not 0: it is not released
	 * onto an elevator car (or a lift) Minecraft cannot feel yet.
	 */
	public static volatile int unresolved;

	private static void refreshInner() {
		int n = SkyLink.active() ? SkyLink.readMovers(READ_IDS, READ_POSE, READ_EPOCH) : -1;
		if (n < 0) {
			unresolved = 0;
			if (snap.n != 0) {
				snap = new Snap(0);
			}
			return;
		}
		Snap old = snap, s = new Snap(n);
		int k = 0;
		for (int i = 0; i < n; i++) {
			int id = READ_IDS[i];
			Integer sid = BINDS.get(id);
			Shape sh = sid == null ? null : SHAPES.get(sid);
			if (sh == null) {
				continue;
			}
			double[] q = READ_POSE;
			Pose p = Pose.of(q[7 * i], q[7 * i + 1], q[7 * i + 2], q[7 * i + 3], q[7 * i + 4], q[7 * i + 5], q[7 * i + 6]);
			int o = old.find(id);
			s.ids[k] = id;
			s.cur[k] = p;
			s.prev[k] = o >= 0 ? old.cur[o] : p;
			s.shape[k] = sh;
			s.bounds[k] = p.place(sh.bounds());
			s.index.put(id, k);
			k++;
		}
		unresolved = n - k;
		if (k != n) {
			Snap t = new Snap(k);
			System.arraycopy(s.ids, 0, t.ids, 0, k);
			System.arraycopy(s.cur, 0, t.cur, 0, k);
			System.arraycopy(s.prev, 0, t.prev, 0, k);
			System.arraycopy(s.shape, 0, t.shape, 0, k);
			System.arraycopy(s.bounds, 0, t.bounds, 0, k);
			t.index.putAll(s.index);
			s = t;
		}
		snap = s;
	}

	public static int count() {
		return snap.n;
	}

	// ---- (bridge #43) mining a mover: the crosshair's ray against the movers' boxes, at their poses ----

	/**
	 * A ray hit on a mover: where (world), the surface normal (world), and the dig lattice cell holding the hit voxel (mover-local: the cell is a 1-block cube
	 * whose corner is the lattice anchor + (cx, cy, cz)). Shapes with no lattice, or marked invisible by the host, are not hit at all. {@code minable} is true.
	 */
	public record Hit(int moverId, int shapeId, double t, double x, double y, double z, double nx, double ny, double nz, int cx, int cy, int cz, boolean minable) {
	}

	/**
	 * The nearest mover box the segment enters from outside, or null. Every mover alike: each shape's boxes in the mover's frame at its pose. A ray that starts
	 * inside a box looks past it (the eye in a mover's solid).
	 */
	public static @org.jspecify.annotations.Nullable Hit pick(Vec3 from, Vec3 to) {
		Snap s = snap;
		if (s.n == 0) {
			return null;
		}
		AABB seg = new AABB(from, to).inflate(0.02);
		double bestT = Double.MAX_VALUE;
		int bestI = -1;
		int bestAxis = 0;
		double bestSign = 1;
		for (int i = 0; i < s.n; i++) {
			if (!s.bounds[i].intersects(seg)) {
				continue;
			}
			float[] lt = LATTICE.get(s.shape[i].id());
			if (lt == null || ((int) lt[0] & Proto.LATTICE_NO_PICK) != 0) {
				continue; // nothing to see or mine there: invisible geometry (a clip brush), or a host that sends no lattice: the ray goes on past it as it always did
			}
			Pose p = s.cur[i];
			Vec3 lf = p.toLocal(from);
			Vec3 ld = p.dirToLocal(to.subtract(from));
			double[] o = {lf.x, lf.y, lf.z};
			double[] d = {ld.x, ld.y, ld.z};
			for (AABB b : s.shape[i].boxes()) {
				double[] lo = {b.minX, b.minY, b.minZ};
				double[] hi = {b.maxX, b.maxY, b.maxZ};
				double enter = 0.0;
				double exit = 1.0;
				int axis = -1;
				double sign = 1;
				boolean ok = true;
				for (int k = 0; k < 3 && ok; k++) {
					if (Math.abs(d[k]) < 1e-12) {
						ok = o[k] >= lo[k] && o[k] <= hi[k];
						continue;
					}
					double t0 = (lo[k] - o[k]) / d[k];
					double t1 = (hi[k] - o[k]) / d[k];
					double sg = d[k] > 0 ? -1 : 1; // the face the ray enters through faces against the ray
					if (t0 > t1) {
						double tmp = t0;
						t0 = t1;
						t1 = tmp;
					}
					if (t0 > enter) {
						enter = t0;
						axis = k;
						sign = sg;
					}
					exit = Math.min(exit, t1);
					ok = enter <= exit;
				}
				if (!ok || axis < 0 || enter >= bestT) {
					continue; // (axis < 0: the ray starts inside the box)
				}
				bestT = enter;
				bestI = i;
				bestAxis = axis;
				bestSign = sign;
			}
		}
		if (bestI < 0) {
			return null;
		}
		Pose p = s.cur[bestI];
		Shape shape = s.shape[bestI];
		Vec3 lf = p.toLocal(from);
		Vec3 ld = p.dirToLocal(to.subtract(from));
		double[] hitL = {lf.x + ld.x * bestT, lf.y + ld.y * bestT, lf.z + ld.z * bestT};
		double[] nl = {0, 0, 0};
		nl[bestAxis] = bestSign;
		Vec3 w = p.toWorld(hitL[0], hitL[1], hitL[2]);
		Vec3 nw = p.dirToWorld(nl[0], nl[1], nl[2]);
		float[] lat = LATTICE.get(shape.id());
		boolean minable = lat != null && ((int) lat[0] & (Proto.LATTICE_NO_PICK | Proto.LATTICE_NO_MINE)) == 0; // (bridge #50) a physics prop / NPC is hit (it blocks the ray) but not minable
		int cx = 0;
		int cy = 0;
		int cz = 0;
		if (lat != null) {
			double in = 0.002; // a little inside the surface: a hit on a plane that is also a lattice plane belongs to the cell the box is in
			cx = (int) Math.floor(hitL[0] - nl[0] * in - lat[1]);
			cy = (int) Math.floor(hitL[1] - nl[1] * in - lat[2]);
			cz = (int) Math.floor(hitL[2] - nl[2] * in - lat[3]);
		}
		return new Hit(s.ids[bestI], shape.id(), bestT, w.x, w.y, w.z, nw.x, nw.y, nw.z, cx, cy, cz, minable);
	}

	/** Tests: one mover with one shape at a pose (identity rotation about the given quaternion), its lattice anchor and flags. */
	static void setForTest(int moverId, int shapeId, AABB[] boxes, double x, double y, double z, double qx, double qy, double qz, double qw, float[] lattice) {
		AABB bounds = boxes[0];
		for (AABB b : boxes) {
			bounds = bounds.minmax(b);
		}
		Shape sh = new Shape(shapeId, boxes, bounds);
		Pose p = Pose.of(x, y, z, qx, qy, qz, qw);
		Snap s = new Snap(1);
		s.ids[0] = moverId;
		s.cur[0] = p;
		s.prev[0] = p;
		s.shape[0] = sh;
		s.bounds[0] = p.place(bounds);
		s.index.put(moverId, 0);
		LATTICE.put(shapeId, lattice);
		snap = s;
	}

	/** Tell the host the player mined a cell of a mover (REN_MOVER_DIG). The host answers with the mover's new shape. */
	public static boolean sendDig(int moverId, int cx, int cy, int cz, int worldId) {
		return sendDig(moverId, cx, cy, cz, worldId, 0);
	}

	/** As above, with the material kind (ChamberMaterial CH_*) the cell was mined as: the walls of its hole are drawn with that block's faces (bridge #50). */
	public static boolean sendDig(int moverId, int cx, int cy, int cz, int worldId, int kind) {
		java.nio.ByteBuffer h = java.nio.ByteBuffer.allocate(Proto.REN_MOVER_DIG_BYTES).order(java.nio.ByteOrder.LITTLE_ENDIAN);
		h.putInt(moverId).putInt(cx).putInt(cy).putInt(cz).putInt(worldId).putInt((kind & 0xFF) << Proto.MOVER_DIG_KIND_SHIFT).flip();
		return SkyLink.writeRender(Proto.REN_MOVER_DIG, h, null);
	}

	/** (bridge #50) The atlas rects {u0 v0 u1 v1} of a material kind's block: side, top, bottom (12 floats), for the walls of holes mined in movers. */
	public static boolean sendStyle(int kind, float[] rects) {
		java.nio.ByteBuffer h = java.nio.ByteBuffer.allocate(Proto.REN_MOVER_DIG_BYTES).order(java.nio.ByteOrder.LITTLE_ENDIAN);
		h.putInt(0).putInt(kind).putInt(0).putInt(0).putInt(0).putInt(Proto.MOVER_DIG_STYLE).flip();
		java.nio.ByteBuffer b = java.nio.ByteBuffer.allocate(Proto.REN_MOVER_STYLE_BYTES).order(java.nio.ByteOrder.LITTLE_ENDIAN);
		for (int i = 0; i < 12; i++) {
			b.putFloat(rects[i]);
		}
		b.flip();
		return SkyLink.writeRender(Proto.REN_MOVER_DIG, h, b);
	}

	// ---- the separating-axis test (adapted from Create (MIT): ContinuousOBBCollider.separateBBs / separate) ----
	// A = a mover box (axis-aligned in the mover's frame), B = the entity's box (axis-aligned in the world, so an oriented
	// box in the mover's frame). The 6 face axes of A and B are tested; Create leaves out the 9 edge-edge axes too (a false
	// contact by an edge at worst). All in the mover's frame: d = B's centre - A's centre, v = B's motion.

	private static final class Sat {
		boolean hit;            // overlapping now, or touching within the motion
		boolean overlap;        // overlapping now
		double toi;             // time of impact in [0, 1) (overlap: 0)
		double nx, ny, nz;      // contact normal (unit, mover frame): B is pushed along it
		double pen = Double.MAX_VALUE;   // overlap: the least penetration, along (px, py, pz)
		double px, py, pz;
		double upPen = Double.MAX_VALUE; // overlap: how far B must rise along world up to clear A (B above A's centre)
	}

	private static double axisR(double[] r, int row, double ex, double ey, double ez) {
		return Math.abs(r[3 * row]) * ex + Math.abs(r[3 * row + 1]) * ey + Math.abs(r[3 * row + 2]) * ez;
	}

	/**
	 * r: the mover's rotation (local -> world, row-major); A: centre ac, half extents ae (local); B: centre bc (local), half
	 * extents be (world axes); v: B's motion (local).
	 */
	private static boolean sat(double[] r, double[] ac, double[] ae, double[] bc, double[] be, double[] v, Sat out) {
		double dx = bc[0] - ac[0], dy = bc[1] - ac[1], dz = bc[2] - ac[2];
		double enter = 0, exit = Double.MAX_VALUE;
		double bnx = 0, bny = 0, bnz = 0;
		boolean overlapAll = true;
		out.pen = Double.MAX_VALUE;
		out.upPen = Double.MAX_VALUE;
		for (int k = 0; k < 6; k++) {
			double axx, axy, axz, tl, ra, rb, pv;
			if (k < 3) {  // A's axes: the mover's local x, y, z
				axx = k == 0 ? 1 : 0;
				axy = k == 1 ? 1 : 0;
				axz = k == 2 ? 1 : 0;
				tl = k == 0 ? dx : k == 1 ? dy : dz;
				ra = ae[k];
				// B's extent on a local axis: sum over B's (world) axes j of |R[j][k]| be[j]
				rb = Math.abs(r[k]) * be[0] + Math.abs(r[3 + k]) * be[1] + Math.abs(r[6 + k]) * be[2];
				pv = v[k];
			} else {  // B's axes: world x, y, z seen in the mover's frame (row j of R)
				int j = k - 3;
				axx = r[3 * j];
				axy = r[3 * j + 1];
				axz = r[3 * j + 2];
				tl = axx * dx + axy * dy + axz * dz;
				ra = axisR(r, j, ae[0], ae[1], ae[2]);
				rb = be[j];
				pv = axx * v[0] + axy * v[1] + axz * v[2];
			}
			double dist = Math.abs(tl), gap = dist - (ra + rb);
			double sgn = tl >= 0 ? 1 : -1;
			if (gap >= -1e-7) {  // apart (or just touching) on this axis now
				overlapAll = false;
				if (pv == 0 || Math.signum(pv) == sgn) {  // not closing on this axis: never meets
					return false;
				}
				double speed = Math.abs(pv);
				if (gap > speed) {
					return false;
				}
				double t0 = Math.max(gap, 0) / speed, t1 = (dist + ra + rb) / speed;
				if (t0 >= enter) {  // the last axis to close is the contact's normal
					enter = t0;
					bnx = sgn * axx;
					bny = sgn * axy;
					bnz = sgn * axz;
				}
				exit = Math.min(exit, t1);
			} else {
				double p = -gap;
				if (pv != 0 && Math.signum(pv) == sgn) {  // overlapping, moving apart: leaves at this time
					exit = Math.min(exit, p / Math.abs(pv));
				}
				if (p < out.pen) {
					out.pen = p;
					out.px = sgn * axx;
					out.py = sgn * axy;
					out.pz = sgn * axz;
				}
				if (k == 4 && tl > 0) {  // world up (B's y axis), B above A's centre
					out.upPen = p;
				}
			}
		}
		out.overlap = overlapAll;
		if (overlapAll) {
			out.hit = true;
			out.toi = 0;
			out.nx = out.px;
			out.ny = out.py;
			out.nz = out.pz;
			return true;
		}
		if (enter > exit || enter >= 1) {
			return false;
		}
		out.hit = true;
		out.toi = enter;
		out.nx = bnx;
		out.ny = bny;
		out.nz = bnz;
		return true;
	}

	// ---- per entity ----

	/** The mover an entity stands on and the mover's pose when it last touched it (the carry is from there). */
	private static final class Contact {
		int mover;
		Pose pose;
		int carriedTick = Integer.MIN_VALUE;
		boolean ground;    // set by this tick's move
		int groundTick = Integer.MIN_VALUE;
	}

	private static final Map<Entity, Contact> CONTACTS = Collections.synchronizedMap(new WeakHashMap<>());

	/** For the local player's McState (bridge #26): the mover it rode in its latest tick and that mover's origin before / now. */
	public static int rideMover, rideFlags;
	public static double ridePrevX, ridePrevY, ridePrevZ, rideCurX, rideCurY, rideCurZ;
	private static Entity rideEntity;
	public static long pushes, carries, sweeps, snaps;
	public static long nanos;  // the time spent here (tests: the cost per tick)

	// ---- (bridge #48) things stuck in a mover: an arrow in a door goes where the door goes ----

	/** Where a stuck thing is on its mover: the point and the direction it points, in the mover's frame. The thing keeps this itself. */
	public record Mount(int mover, Vec3 point, Vec3 dir) {
	}

	/** A mounted thing's world position and direction. */
	public record Placed(Vec3 pos, Vec3 dir) {
	}

	/** The mount of a point and direction (world) on this mover at its pose now, or null when the mover isn't there. */
	public static @org.jspecify.annotations.Nullable Mount mount(int moverId, Vec3 pos, Vec3 dir) {
		Snap s = snap;
		int i = s.find(moverId);
		if (i < 0) {
			return null;
		}
		Pose p = s.cur[i];
		return new Mount(moverId, p.toLocal(pos), p.dirToLocal(dir));
	}

	/** Where the mount is now, or null when its mover is gone (removed, a map change). */
	public static @org.jspecify.annotations.Nullable Placed place(Mount m) {
		Snap s = snap;
		int i = s.find(m.mover());
		if (i < 0) {
			return null;
		}
		Pose p = s.cur[i];
		return new Placed(p.toWorld(m.point().x, m.point().y, m.point().z), p.dirToWorld(m.dir().x, m.dir().y, m.dir().z));
	}

	/** (bridge #36) A teleport ends whatever the entity stood on: its carry (the old mover's motion applied at the new spot) and its ride. */
	public static void forget(Entity e) {
		CONTACTS.remove(e);
		if (e == rideEntity) {
			rideMover = 0;
			rideFlags = 0;
		}
	}

	/** The local player is the one whose ride is reported (set once per client tick). */
	public static void setRideEntity(Entity e) {
		rideEntity = e;
	}

	private static boolean applies(Entity e) {
		return !e.noPhysics && !(e instanceof ServerPlayer) && !e.isPassenger() && SkyLink.active();
	}

	private static final double EPS = 1e-4;

	/**
	 * (bridge #72) A mover's boxes with the holes of the open portals cut out, for an entity that can enter them (the same rule as the blocks'
	 * and the host voxels', HostShapes.carve). A portal on a floor over a thin mover slab (Portal 2's invisible catch plate 16 u under a pit
	 * floor) must let the body down to where its eye crosses the plane. Exact for a mover whose frame is axis-aligned (every static floor and wall
	 * brush; doors at rest); a turned mover keeps its boxes whole. {@code boxes} itself when no hole touches it.
	 */
	private static AABB[] carved(AABB[] boxes, Pose cur, AABB @org.jspecify.annotations.Nullable [] holes) {
		if (holes == null || holes.length == 0 || !axisAligned(cur.r)) {
			return boxes;
		}
		ArrayList<AABB> out = null;
		for (int bi = 0; bi < boxes.length; bi++) {
			AABB b = boxes[bi];
			AABB w = cur.place(b);
			boolean hit = false;
			for (AABB h : holes) {
				hit = hit || h.intersects(w);
			}
			if (!hit) {
				if (out != null) {
					out.add(b);
				}
				continue;
			}
			if (out == null) {
				out = new ArrayList<>(boxes.length + 8);
				for (int j = 0; j < bi; j++) {
					out.add(boxes[j]);
				}
			}
			ArrayList<AABB> pieces = new ArrayList<>(4);
			pieces.add(w);
			for (AABB h : holes) {
				ArrayList<AABB> next = new ArrayList<>(pieces.size() + 4);
				for (AABB p : pieces) {
					subtract(p, h, next);
				}
				pieces = next;
			}
			for (AABB p : pieces) {
				Vec3 a = cur.toLocal(new Vec3(p.minX, p.minY, p.minZ)), c = cur.toLocal(new Vec3(p.maxX, p.maxY, p.maxZ));
				out.add(new AABB(Math.min(a.x, c.x), Math.min(a.y, c.y), Math.min(a.z, c.z), Math.max(a.x, c.x), Math.max(a.y, c.y), Math.max(a.z, c.z)));
			}
		}
		return out == null ? boxes : out.toArray(new AABB[0]);
	}

	private static boolean axisAligned(double[] r) {
		for (double v : r) {
			double a = Math.abs(v);
			if (a > 1e-3 && a < 1 - 1e-3) {
				return false;
			}
		}
		return true;
	}

	/** p minus h as up to six boxes appended to out (p whole when they do not overlap). */
	private static void subtract(AABB p, AABB h, ArrayList<AABB> out) {
		if (!p.intersects(h)) {
			out.add(p);
			return;
		}
		double x0 = p.minX, x1 = p.maxX, y0 = p.minY, y1 = p.maxY;
		if (h.minX > p.minX) {
			out.add(new AABB(p.minX, p.minY, p.minZ, h.minX, p.maxY, p.maxZ));
			x0 = h.minX;
		}
		if (h.maxX < p.maxX) {
			out.add(new AABB(h.maxX, p.minY, p.minZ, p.maxX, p.maxY, p.maxZ));
			x1 = h.maxX;
		}
		if (h.minY > p.minY) {
			out.add(new AABB(x0, p.minY, p.minZ, x1, h.minY, p.maxZ));
			y0 = h.minY;
		}
		if (h.maxY < p.maxY) {
			out.add(new AABB(x0, h.maxY, p.minZ, x1, p.maxY, p.maxZ));
			y1 = h.maxY;
		}
		if (h.minZ > p.minZ) {
			out.add(new AABB(x0, y0, p.minZ, x1, y1, h.minZ));
		}
		if (h.maxZ < p.maxZ) {
			out.add(new AABB(x0, y0, h.maxZ, x1, y1, p.maxZ));
		}
	}

	/**
	 * Head of Entity.move: carry the entity on the mover it stands on, push it out of movers that moved into it, and
	 * return its movement clipped against the movers (exact, in each mover's frame). Vanilla then collides that against
	 * the blocks and the host's static field.
	 */
	public static Vec3 beforeMove(Entity e, Vec3 mv) {
		Snap s = snap;
		if (s.n == 0 || !applies(e)) {
			if (e == rideEntity) {
				rideMover = 0;
				rideFlags = 0;
			}
			return mv;
		}
		long t0 = System.nanoTime();
		Contact c = CONTACTS.get(e);
		boolean isRide = e == rideEntity;
		int carriedBy = 0;
		if (isRide) {
			rideFlags = 0;
		}
		// 1. Carry (once per tick): the contact point moves with the mover from its pose when the entity last stood on it to
		//    its pose now (Create's getContactPointMotion; PhysX "ride on touched object")
		if (c != null && c.ground && c.carriedTick != e.tickCount && c.groundTick >= e.tickCount - 2) {
			c.carriedTick = e.tickCount;
			int i = s.find(c.mover);
			if (i >= 0 && c.pose != null) {
				Pose now = s.cur[i];
				Vec3 p = e.position();
				Vec3 l = c.pose.toLocal(p);
				Vec3 w = now.toWorld(l.x, l.y, l.z);
				Vec3 d = w.subtract(p);
				if (isRide) {
					ridePrevX = c.pose.x;
					ridePrevY = c.pose.y;
					ridePrevZ = c.pose.z;
				}
				// (bridge #31) a mover that only turns under a rider standing on its pivot moves the contact point by nothing, but it carries
				// the rider all the same (the view turns with it): the ride counts whenever the pose changed
				if (isRide && !c.pose.same(now) && d.lengthSqr() < 16) {
					rideFlags |= Proto.RIDE_MOVED;
				}
				if (d.lengthSqr() > 1e-12 && d.lengthSqr() < 16) {
					Vec3 allowed = Entity.collideBoundingBox(e, d, e.getBoundingBox(), e.level(), List.of());
					e.setPos(p.x + allowed.x, p.y + allowed.y, p.z + allowed.z);
					carries++;
					if (isRide) {
						rideFlags |= Proto.RIDE_MOVED;
					}
				}
				carriedBy = c.mover;
				c.pose = now;
			}
		}
		AABB[] holes = HostShapes.holesFor(e);  // (bridge #72) the open portals' holes this entity can enter: cut out of every mover's boxes
		AABB eb = e.getBoundingBox();
		// (Create: the player a little shorter, so it is less likely to stick under a ceiling)
		if (e.getBbHeight() > 1) {
			eb = eb.contract(0, 2 / 16.0, 0);
		}
		double step = e.maxUpStep();
		boolean wasGround = e.onGround() || (c != null && c.ground);
		AABB reach = eb.expandTowards(mv).inflate(0.25);
		Sat sat = new Sat();
		double[] ac = new double[3], ae = new double[3], bc = new double[3], be = {(eb.maxX - eb.minX) * 0.5, (eb.maxY - eb.minY) * 0.5, (eb.maxZ - eb.minZ) * 0.5};
		double[] v = new double[3];
		// 2. Overlaps (a mover moved into the entity, or the entity was put inside one): swept from the mover's previous pose
		//    when it was clear then (it pushes the entity ahead of it, however fast), else out along the least penetration,
		//    or up onto a top within step height
		Vec3 push = Vec3.ZERO;
		for (int i = 0; i < s.n; i++) {
			if (!s.bounds[i].intersects(reach)) {
				continue;
			}
			Pose cur = s.cur[i], prev = s.prev[i];
			AABB[] boxes = carved(s.shape[i].boxes(), cur, holes);
			for (int iter = 0; iter < 3; iter++) {
				Vec3 centre = eb.getCenter().add(push);
				Vec3 lc = cur.toLocal(centre);
				double bestPen = 0;
				Vec3 bestSep = null;
				for (AABB b : boxes) {
					fill(b, ac, ae);
					bc[0] = lc.x;
					bc[1] = lc.y;
					bc[2] = lc.z;
					v[0] = v[1] = v[2] = 0;
					if (!sat(cur.r, ac, ae, bc, be, v, sat) || !sat.overlap) {
						continue;
					}
					Vec3 sep = null;
					if (s.ids[i] != carriedBy && !prev.same(cur)) {
						// where the entity was relative to the mover before it moved: swept to now, the entity stops at contact
						Vec3 lp = prev.toLocal(centre);
						bc[0] = lp.x;
						bc[1] = lp.y;
						bc[2] = lp.z;
						v[0] = lc.x - lp.x;
						v[1] = lc.y - lp.y;
						v[2] = lc.z - lp.z;
						Sat sw = new Sat();
						if (sat(cur.r, ac, ae, bc, be, v, sw) && !sw.overlap) {
							double t = Math.max(0, sw.toi - EPS);
							Vec3 at = cur.toWorld(lp.x + v[0] * t, lp.y + v[1] * t, lp.z + v[2] * t);
							sep = at.subtract(centre).add(cur.dirToWorld(sw.nx, sw.ny, sw.nz).scale(EPS));
						}
					}
					if (sep == null) {
						if (sat.upPen <= step + 1e-6 && wasGround) {
							sep = new Vec3(0, sat.upPen + EPS, 0);  // onto its top (the step)
						} else {
							sep = cur.dirToWorld(sat.px, sat.py, sat.pz).scale(sat.pen + EPS);
						}
					}
					if (sep.lengthSqr() > bestPen) {
						bestPen = sep.lengthSqr();
						bestSep = sep;
					}
				}
				if (bestSep == null) {
					break;
				}
				push = push.add(bestSep);
			}
		}
		if (push.lengthSqr() > 1e-12) {
			Vec3 allowed = Entity.collideBoundingBox(e, push, e.getBoundingBox(), e.level(), List.of());
			Vec3 p = e.position();
			e.setPos(p.x + allowed.x, p.y + allowed.y, p.z + allowed.z);
			eb = eb.move(allowed);
			pushes++;
			if (isRide) {
				rideFlags |= Proto.RIDE_PUSHED;
			}
		}
		// 3. The entity's own motion, swept against every mover box near it (its time of impact, Create's temporal
		//    response); slides along what it hits, up to 3 times. A wall low enough to step onto is let through: the
		//    overlap pass above lifts the entity onto it next move, as vanilla's step does.
		Vec3 rem = mv, total = Vec3.ZERO;
		Vec3 dm = e.getDeltaMovement();
		int groundMover = 0;
		Pose groundPose = null;
		for (int iter = 0; iter < 4 && rem.lengthSqr() > 1e-14; iter++) {
			double best = 1;
			Vec3 bestN = null;
			int bestMover = -1;
			AABB sweep = eb.move(total).expandTowards(rem).inflate(EPS);
			for (int i = 0; i < s.n; i++) {
				if (!s.bounds[i].intersects(sweep)) {
					continue;
				}
				Pose cur = s.cur[i];
				Vec3 lc = cur.toLocal(eb.getCenter().add(total));
				Vec3 lv = cur.dirToLocal(rem);
				for (AABB b : carved(s.shape[i].boxes(), cur, holes)) {
					fill(b, ac, ae);
					bc[0] = lc.x;
					bc[1] = lc.y;
					bc[2] = lc.z;
					v[0] = lv.x;
					v[1] = lv.y;
					v[2] = lv.z;
					if (!sat(cur.r, ac, ae, bc, be, v, sat) || sat.overlap || sat.toi >= best) {
						continue;
					}
					Vec3 n = cur.dirToWorld(sat.nx, sat.ny, sat.nz);
					if (Math.abs(n.y) < 0.5 && wasGround) {
						double top = cur.place(b).maxY;
						if (top - eb.move(total).minY <= step + 1e-6) {
							continue;  // a step: lifted onto it by the overlap pass
						}
					}
					best = sat.toi;
					bestN = n;
					bestMover = i;
				}
			}
			if (bestN == null) {
				total = total.add(rem);
				break;
			}
			sweeps++;
			double t = Math.max(0, best - EPS / Math.max(rem.length(), EPS));
			total = total.add(rem.scale(t));
			Vec3 left = rem.scale(1 - t);
			double into = left.dot(bestN);
			rem = into < 0 ? left.subtract(bestN.scale(into)) : left;
			double dmInto = dm.dot(bestN);
			if (dmInto < 0) {
				dm = dm.subtract(bestN.scale(dmInto));
			}
			if (bestN.y > 0.7) {
				groundMover = s.ids[bestMover];
				groundPose = s.cur[bestMover];
			}
		}
		// standing on a mover with no downward motion this move (or a sweep that ended just above it): a short probe down.
		// Ground snapping (as character controllers do): an entity that stood on a mover within the last 2 ticks keeps to its
		// top up to 0.6 block below (a descending car moved on while the entity was held for a tick: a teleport's hold)
		boolean snap = c != null && c.groundTick >= e.tickCount - 2 && mv.y <= 0 && e.getDeltaMovement().y < 0.05;
		if (groundMover == 0 && mv.y <= 0) {
			AABB at = eb.move(total);
			Vec3 lv0 = new Vec3(0, snap ? -0.6 : -0.04, 0);
			for (int i = 0; i < s.n && groundMover == 0; i++) {
				if (!s.bounds[i].intersects(at.expandTowards(lv0))) {
					continue;
				}
				Pose cur = s.cur[i];
				Vec3 lc = cur.toLocal(at.getCenter());
				Vec3 lv = cur.dirToLocal(lv0);
				for (AABB b : carved(s.shape[i].boxes(), cur, holes)) {
					fill(b, ac, ae);
					bc[0] = lc.x;
					bc[1] = lc.y;
					bc[2] = lc.z;
					v[0] = lv.x;
					v[1] = lv.y;
					v[2] = lv.z;
					if (sat(cur.r, ac, ae, bc, be, v, sat) && !sat.overlap && cur.dirToWorld(sat.nx, sat.ny, sat.nz).y > 0.7) {
						groundMover = s.ids[i];
						groundPose = cur;
						if (snap && sat.toi * 0.6 > 0.04) {  // (down onto it)
							total = total.add(0, Math.min(0, lv0.y * sat.toi + EPS), 0);
							snaps++;
						}
						break;
					}
				}
			}
		}
		if (groundMover != 0 && dm.y < 0) {
			dm = new Vec3(dm.x, 0, dm.z);
		}
		if (!dm.equals(e.getDeltaMovement())) {
			e.setDeltaMovement(dm);
		}
		if (groundMover != 0) {
			if (c == null) {
				c = new Contact();
				CONTACTS.put(e, c);
			}
			if (c.mover != groundMover) {
				c.carriedTick = e.tickCount;  // (the carry starts from the next tick)
			}
			c.mover = groundMover;
			c.pose = groundPose;
			c.ground = true;
			c.groundTick = e.tickCount;
			e.resetFallDistance();
		} else if (c != null) {
			c.ground = false;
		}
		if (isRide) {
			rideMover = groundMover != 0 ? groundMover : carriedBy;
			if (rideMover != 0) {
				int i = s.find(rideMover);
				Pose p = i >= 0 ? s.cur[i] : null;
				if (p != null) {
					rideCurX = p.x;
					rideCurY = p.y;
					rideCurZ = p.z;
					if (carriedBy != 0 && rideMover != carriedBy) {
						// (bridge #40) it stepped from the mover that carried it (the arrival car, which has just stopped) onto another one (the floor beside it):
						// the previous tick's origin belongs to the old mover, and the host's interpolation of the feet in the mover's frame ran from the
						// local position on the old one to the local position on the new one: a dip of 15 .. 70 u for a frame or two at the car's stop
						ridePrevX = p.x;
						ridePrevY = p.y;
						ridePrevZ = p.z;
					} else if ((rideFlags & Proto.RIDE_MOVED) == 0) {
						ridePrevX = p.x;
						ridePrevY = p.y;
						ridePrevZ = p.z;
					}
				}
			}
		}
		if (isRide && debugTicks > 0 && e.tickCount != debugLastTick) {
			debugLastTick = e.tickCount;
			debugTicks--;
			SkyCraft.LOG.info("SkyCraft: movers tick {} y {} carried by {} (c {} ground {} gt {}) push {} mv.y {} -> {} ground {} posed {}", e.tickCount, String.format("%.3f", e.getY()), carriedBy,
				c == null ? 0 : c.mover, c != null && c.ground, c == null ? -1 : c.groundTick, String.format("%.3f %.3f %.3f", push.x, push.y, push.z), String.format("%.3f", mv.y),
				String.format("%.3f", total.y), groundMover, s.n);
		}
		nanos += System.nanoTime() - t0;
		return total;
	}

	/** (tests) log this many local player ticks (SKYCRAFT movers debug: -Dportalcraft.moversDebug or the first ones of a session). */
	public static int debugTicks = Integer.getInteger("portalcraft.moversDebug", 400);
	private static int debugLastTick = Integer.MIN_VALUE;

	/**
	 * A held position (a teleport waiting for its collision) of an entity standing on a mover moves with that mover between
	 * the last client tick's pose and this one's: the hold is in the mover's frame (an elevator descending under a player held
	 * for a tick left it in the air).
	 */
	public static Vec3 carryHeld(Entity e, Vec3 hold) {
		Snap s = snap;
		Contact c = CONTACTS.get(e);
		if (c == null || !c.ground || c.groundTick < e.tickCount - 2) {
			return hold;
		}
		int i = s.find(c.mover);
		if (i < 0 || s.prev[i].same(s.cur[i])) {
			return hold;
		}
		Vec3 l = s.prev[i].toLocal(hold);
		Vec3 w = s.cur[i].toWorld(l.x, l.y, l.z);
		return w.distanceToSqr(hold) < 16 ? w : hold;
	}

	/**
	 * (bridge #40) A hand-over hold ends on the mover the body stands on: the hold position is the body's, and the mover's pose here is a few ms newer or older,
	 * so the player is up to 10+ u above the floor of a car that descends (it then hung in the air and fell onto the car late: the visible fall) or below
	 * the floor of one that rises (the lift of liftOutOfGeometry covers 0.75 block of it, no more). Put the player exactly on the top it is nearest to, within
	 * 0.75 block either way, and make that the entity's contact so the next tick's carry moves it with the mover.
	 */
	public static Vec3 standOnRelease(Entity e, Vec3 pos) {
		Snap s = snap;
		double best = Double.NaN;
		int bestI = -1;
		for (int i = 0; i < s.n; i++) {
			AABB wb = s.bounds[i];
			if (pos.x < wb.minX || pos.x > wb.maxX || pos.z < wb.minZ || pos.z > wb.maxZ || wb.maxY < pos.y - 0.75 || wb.minY > pos.y + 0.75) {
				continue;
			}
			for (AABB b : s.shape[i].boxes()) {
				AABB w = s.cur[i].place(b);
				if (pos.x > w.minX && pos.x < w.maxX && pos.z > w.minZ && pos.z < w.maxZ && w.maxY >= pos.y - 0.75 && w.maxY <= pos.y + 0.75
					&& (Double.isNaN(best) || Math.abs(w.maxY - pos.y) < Math.abs(best - pos.y))) {
					best = w.maxY;
					bestI = i;
				}
			}
		}
		if (bestI < 0) {
			return pos;
		}
		Contact c = new Contact();
		c.mover = s.ids[bestI];
		c.pose = s.cur[bestI];
		c.ground = true;
		c.groundTick = e.tickCount;
		c.carriedTick = e.tickCount;
		CONTACTS.put(e, c);
		return new Vec3(pos.x, best + 1e-3, pos.z);
	}

	/** Return of Entity.move: an entity on a mover is on the ground (vanilla saw no block under it). */
	public static void afterMove(Entity e) {
		if (snap.n == 0) {
			return;
		}
		Contact c = CONTACTS.get(e);
		if (c != null && c.ground && c.groundTick == e.tickCount) {
			e.setOnGround(true);
			e.verticalCollision = true;
			e.verticalCollisionBelow = true;
			e.resetFallDistance();
		}
	}

	private static void fill(AABB b, double[] c, double[] h) {
		c[0] = (b.minX + b.maxX) * 0.5;
		c[1] = (b.minY + b.maxY) * 0.5;
		c[2] = (b.minZ + b.maxZ) * 0.5;
		h[0] = (b.maxX - b.minX) * 0.5;
		h[1] = (b.maxY - b.minY) * 0.5;
		h[2] = (b.maxZ - b.minZ) * 0.5;
	}

	// ---- queries ----

	/** The highest mover top under (x, z) between y - depth and y + up (world bounds of its boxes), or NaN. */
	public static double topNear(double x, double y, double z, double depth, double up) {
		Snap s = snap;
		double best = Double.NaN;
		for (int i = 0; i < s.n; i++) {
			AABB wb = s.bounds[i];
			if (x < wb.minX || x > wb.maxX || z < wb.minZ || z > wb.maxZ || wb.maxY < y - depth || wb.minY > y + up) {
				continue;
			}
			for (AABB b : s.shape[i].boxes()) {
				AABB w = s.cur[i].place(b);
				if (x > w.minX && x < w.maxX && z > w.minZ && z < w.maxZ && w.maxY >= y - depth && w.maxY <= y + up && !(w.maxY < best)) {
					best = w.maxY;
				}
			}
		}
		return best;
	}

	/** (bridge #53) the sizes of this class's static caches and queues, for the periodic growth line in the log. */
	public static String growth() {
		return "mover shapes " + SHAPES.size() + ", binds " + BINDS.size() + ", lattices " + LATTICE.size() + ", contacts " + CONTACTS.size();
	}
}
