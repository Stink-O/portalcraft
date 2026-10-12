package dev.skycraft.world;

import net.minecraft.world.phys.AABB;
import net.minecraft.world.phys.Vec3;
import org.jetbrains.annotations.Nullable;

/**
 * Portalcraft (bridge #22): the host's linked portal pair in Minecraft coordinates (kInPortal), so Minecraft's own
 * mobs, dropped items and projectiles go through it as Portal 2's props do: one walking into the cliff portal comes
 * out of its partner in the chamber, and back. Portal 2's rule: a point that has passed portal i's plane is carried
 * to the same spot relative to portal j, turned half a turn about the up axis (in front of i = behind j and back).
 * Both portals are written by the client thread (InputBridge) and read on the server thread.
 */
public final class PortalPair {
	/** Half the opening, in blocks (Portal 2's portal: 64 x 112 units, 40 units a block). */
	public static final double HALF_W = 0.8, HALF_H = 1.4;

	/**
	 * Bridge #32: portal SLOTS, slot = linkage group index * 2 + colour (the host's bridge.hpp kPortalSlots); a portal's partner is slot ^ 1.
	 * Group 0 is the gun's pair (slots 0 and 1); every other group of the map (pre-placed pairs) has slots of its own, and each
	 * linked pair carries mobs, items and the player.
	 */
	public static final int SLOTS = 16;

	/** Bridge #45: the mobs' hole starts this far in front of the plane (blocks): the host's exported 5 u voxels can stand up to 5 u in front of a wall's true surface. */
	private static final double FRONT = 0.15;

	private static final double[][] PENDING = new double[SLOTS][9];
	/** (bridge #68) half the opening of each slot, blocks: a linked_portal_door is 64 x 64 or 64 x 128 units, not the gun's 64 x 112. Pose element 4 = (halfW, halfH, 0). */
	private static final double[][] SIZE = new double[SLOTS][2];

	public static double halfW(Vec3[] pose) {
		return pose.length > 4 ? pose[4].x : HALF_W;
	}

	public static double halfH(Vec3[] pose) {
		return pose.length > 4 ? pose[4].y : HALF_H;
	}
	private static final Vec3[][] LIVE = new Vec3[SLOTS][];  // centre, normal, up, right
	private static volatile AABB[] holes = new AABB[SLOTS];
	private static volatile boolean linked, anyLinked;
	private static int logs;

	private PortalPair() {
	}

	public static synchronized void set(int code, double a, double b, double c) {
		int group = (code >> 8) & 0xFF;  // (bridge #32: the linkage group index; 0 from older hosts)
		if (group >= SLOTS / 2) {
			return;
		}
		if ((code & 0x10) != 0) {
			LIVE[group * 2] = LIVE[group * 2 + 1] = null;
			SIZE[group * 2][0] = SIZE[group * 2][1] = SIZE[group * 2 + 1][0] = SIZE[group * 2 + 1][1] = 0;
			update();
			return;
		}
		int i = (code & 1) | (group << 1), part = (code >> 1) & 3;
		if (part == 3) {  // (bridge #68) the opening's half size, in thousandths of a block
			SIZE[i][0] = a * 1e-3;
			SIZE[i][1] = b * 1e-3;
			Vec3[] cur = LIVE[i];
			if (cur != null && SIZE[i][0] > 0.05 && SIZE[i][1] > 0.05) {
				LIVE[i] = new Vec3[] { cur[0], cur[1], cur[2], cur[3], new Vec3(SIZE[i][0], SIZE[i][1], 0) };
				update();
			}
			return;
		}
		double k = part == 0 ? 1e-3 : 1e-6;
		PENDING[i][part * 3] = a * k;
		PENDING[i][part * 3 + 1] = b * k;
		PENDING[i][part * 3 + 2] = c * k;
		if (part == 2) {
			double[] p = PENDING[i];
			Vec3 n = new Vec3(p[3], p[4], p[5]).normalize(), u = new Vec3(p[6], p[7], p[8]).normalize();
			Vec3[] old = LIVE[i];
			LIVE[i] = SIZE[i][0] > 0.05 && SIZE[i][1] > 0.05
					? new Vec3[] { new Vec3(p[0], p[1], p[2]), n, u, n.cross(u), new Vec3(SIZE[i][0], SIZE[i][1], 0) }
					: new Vec3[] { new Vec3(p[0], p[1], p[2]), n, u, n.cross(u) };
			if (logs < 20 && (old == null || old[0].distanceToSqr(LIVE[i][0]) > 1e-4)) {
				logs++;
				dev.skycraft.SkyCraft.LOG.info("Portalcraft: portal {} at {} normal {} up {} (mobs and items go through)", i, LIVE[i][0], n, u);
			}
			update();
		}
	}

	private static void update() {
		AABB[] h = new AABB[SLOTS];
		boolean any = false;
		for (int i = 0; i < SLOTS; i++) {
			if (LIVE[i] == null || LIVE[i ^ 1] == null) {
				continue;
			}
			any = true;
			h[i] = holeBox(LIVE[i], 2.0);
		}
		holes = h;
		linked = LIVE[0] != null && LIVE[1] != null;
		anyLinked = any;
	}

	/** The gun's pair (group 0) is live. */
	public static boolean linked() {
		return linked;
	}

	/** Any pair of the map is live (bridge #32). */
	public static boolean anyLinked() {
		return anyLinked;
	}

	/** Portal i's pose {centre, normal, up, right}, or null. */
	public static synchronized Vec3 @Nullable [] pose(int i) {
		return i >= 0 && i < SLOTS ? LIVE[i] : null;
	}

	/** Signed distance of p in front of portal i's plane (blocks). */
	public static double depth(Vec3[] pose, Vec3 p) {
		return p.subtract(pose[0]).dot(pose[1]);
	}

	/** p (on the plane, or near it) is inside portal i's opening, scaled by s. */
	public static boolean inOpening(Vec3[] pose, Vec3 p, double s) {
		Vec3 q = p.subtract(pose[0]);
		double x = q.dot(pose[3]) / (halfW(pose) * s), y = q.dot(pose[2]) / (halfH(pose) * s);
		return x * x + y * y <= 1.0;
	}

	/** p (on the plane, or near it) is inside the hole carved behind the opening (its rectangle). */
	public static boolean inHole(Vec3[] pose, Vec3 p) {
		Vec3 q = p.subtract(pose[0]);
		return Math.abs(q.dot(pose[3])) <= halfW(pose) * 0.95 + 0.05 && Math.abs(q.dot(pose[2])) <= halfH(pose) * 0.95 + 0.05;
	}

	/** A point carried from portal a's side to portal b's (Portal 2's transform). */
	public static Vec3 point(Vec3[] a, Vec3[] b, Vec3 p) {
		return b[0].add(dir(a, b, p.subtract(a[0])));
	}

	/** A direction (velocity, look) carried through the pair: no translation. */
	public static Vec3 dir(Vec3[] a, Vec3[] b, Vec3 d) {
		double f = d.dot(a[1]), r = d.dot(a[3]), u = d.dot(a[2]);
		return b[1].scale(-f).add(b[3].scale(-r)).add(b[2].scale(u));
	}

	/**
	 * (bridge #48) The same hole, {@code depth} blocks deep behind the plane (a projectile that covers more than the 2 blocks in a tick must not
	 * meet the wall behind the opening before EntityTransit sees it cross the plane), or null.
	 */
	public static synchronized @Nullable AABB hole(int i, double depth) {
		if (i < 0 || i >= SLOTS || LIVE[i] == null || LIVE[i ^ 1] == null) {
			return null;
		}
		return holeBox(LIVE[i], depth);
	}

	/** The opening's box (95% of it), from FRONT in front of the plane to {@code depth} behind it. */
	private static AABB holeBox(Vec3[] p, double depth) {
		Vec3 c = p[0], n = p[1], u = p[2].scale(halfH(p) * 0.95), r = p[3].scale(halfW(p) * 0.95);
		AABB box = null;
		for (int k = 0; k < 8; k++) {
			Vec3 q = c.add(u.scale((k & 1) == 0 ? -1 : 1)).add(r.scale((k & 2) == 0 ? -1 : 1)).add(n.scale((k & 4) == 0 ? FRONT : -depth));
			box = box == null ? new AABB(q, q) : box.minmax(new AABB(q, q));
		}
		return box;
	}

	/** The hole behind portal i's opening for mobs and items (the opening's box, 2 blocks deep behind the plane), or null. */
	public static @Nullable AABB hole(int i) {
		return i >= 0 && i < SLOTS ? holes[i] : null;
	}
}
