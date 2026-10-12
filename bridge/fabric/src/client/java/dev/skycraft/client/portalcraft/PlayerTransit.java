package dev.skycraft.client.portalcraft;

import dev.skycraft.SkyCraft;
import dev.skycraft.link.SkyLink;
import dev.skycraft.portalcraft.PlayerFling;
import dev.skycraft.world.PortalPair;
import dev.skycraft.world.SkyCollision;
import net.minecraft.client.Minecraft;
import net.minecraft.client.player.LocalPlayer;
import net.minecraft.core.BlockPos;
import net.minecraft.world.phys.AABB;
import net.minecraft.world.phys.Vec3;

/**
 * Portalcraft (bridge #28): Minecraft transits its own player through the host's portal pair (PortalPair, kInPortal), as it does
 * for mobs (EntityTransit). The approach is Immersive Portals' (qouteall, iPortalTeam/ImmersivePortalsMod, Apache-2.0,
 * ClientTeleportationManager): the player goes through when its EYE crosses the portal's plane inside the opening, and both the
 * position of this tick and that of the last tick are carried through the pair, so the eye's path (and anything interpolated
 * between the two ticks) is one straight line through the portal: the host draws the part of the tick still behind the exit's plane
 * through the pair (mctransit.cpp), and the camera is continuous at every frame with no blend and no hold.
 * <p>
 * Per crossing, at the end of the client tick:
 * <ul>
 * <li>position: the eye of both ticks through the pair, the body upright under it;</li>
 * <li>velocity: turned by the pair, with Portal 2's gravity fix-up (the gravity gained after the crossing is taken off before the
 * turn and given back after it), then Portal 2's exit speed rules (research/specs/portal-transit.md 1.3-1.4): floor exits push
 * out at 300 u/s at least (for the feet: the eye, not the hull centre, is at the plane here), tilted exits get Portal 2's arc speed,
 * anything else is capped at 1000 u/s;</li>
 * <li>look: turned by the pair. The host turns its own look (Portal 2's view angles) when it sees the transit and acks it in SkyState;
 * until then {@link #look} turns the host's look here;</li>
 * <li>the fling (PlayerFling): Portal 2's air physics until the player lands, and no fall damage for the transit or that landing;</li>
 * <li>the server's copy of the player is put at the exit by the first movement packet from there (PlayerTransitServerMixin).</li>
 * </ul>
 * McState carries transitSeq, the portals and the transform (McTransit) to the host.
 */
public final class PlayerTransit {
	private static int seq, logs, heldLogs, freeLogs;
	private static final float[][] PENDING_Q = new float[8][];
	private static final int[] PENDING_SEQ = new int[8];

	private PlayerTransit() {
	}

	public static int seq() {
		return seq;
	}

	/** End of the client tick, before McState is published. */
	public static void tick(Minecraft minecraft, SkyLink.McState mc, boolean held) {
		LocalPlayer player = minecraft.player;
		if (player == null) {
			PlayerFling.end();
			return;
		}
		// the fling ends when the player has slid down to a sprint on the ground, swims, climbs, flies, glides, rides or dies
		Vec3 dm = player.getDeltaMovement();
		boolean slowOnGround = player.onGround() && dm.x * dm.x + dm.z * dm.z <= PlayerFling.SLIDE_END * PlayerFling.SLIDE_END;
		if (PlayerFling.flinging(player) && !PlayerFling.beam && (slowOnGround || player.isInWater() || player.isInLava() || player.onClimbable() || player.getAbilities().flying
			|| player.isFallFlying() || player.isPassenger() || player.isDeadOrDying())) {
			PlayerFling.end();
			if (logs < 200) {
				logs++;
				SkyCraft.LOG.info("Portalcraft: player transit {}: fling ended at {} ({}), velocity {}", seq, fmt(player.position()), player.onGround() ? "landed" : "water/ladder/flying",
					fmt(player.getDeltaMovement()));
			}
		} else if (PlayerFling.flinger != null && PlayerFling.flinger != player) {
			PlayerFling.end();
		}
		if (held || !AvatarHost.hostWantsTransit() || !PortalPair.anyLinked() || player.isPassenger() || player.isSpectator() || player.isDeadOrDying()) {
			return;
		}
		double eyeH = player.getEyeHeight();
		Vec3 e0 = new Vec3(player.xo, player.yo + eyeH, player.zo), e1 = player.position().add(0, eyeH, 0);
		if (e0.distanceToSqr(e1) > 64.0) {
			return;  // a teleport, not a move
		}
		// every linked pair of the map (bridge #32): slot i's partner is i ^ 1 (the other colour of its linkage group)
		int in = -1;
		double best = 2.0;
		for (int i = 0; i < PortalPair.SLOTS; i++) {
			Vec3[] a = PortalPair.pose(i);
			if (a == null || PortalPair.pose(i ^ 1) == null) {
				continue;
			}
			double d0 = PortalPair.depth(a, e0), d1 = PortalPair.depth(a, e1);
			if (!(d0 >= 0.0 && d1 < 0.0)) {
				continue;
			}
			double t = d0 / (d0 - d1);
			if (t < best && PortalPair.inHole(a, e0.add(e1.subtract(e0).scale(t)))) {
				best = t;
				in = i;
			}
		}
		if (in >= 0) {
			transit(minecraft, player, mc, in, PortalPair.pose(in), PortalPair.pose(in ^ 1), best, e0, e1, eyeH);
		}
	}

	private static void transit(Minecraft minecraft, LocalPlayer player, SkyLink.McState mc, int in, Vec3[] a, Vec3[] b, double t, Vec3 e0, Vec3 e1, double eyeH) {
		Vec3 up = new Vec3(0, eyeH, 0);
		// The last tick's end goes through the pair by the eye (the camera's path is one line through the portal); this tick's by
		// the hull centre, as Portal 2 places Chell (portal-transit.md 1.5): a pair that turns "up" (floor <-> wall) would put an
		// upright body hanging from the eye 0.7 block lower than Chell's (it landed at once out of a wall portal, and rose too low
		// out of a floor portal). The camera eases up that difference over the rest of this tick: no step in any frame.
		Vec3 half = new Vec3(0, player.getBbHeight() * 0.5, 0);
		Vec3 prevFeet = PortalPair.point(a, b, e0).subtract(up);
		// (only the height: the hull centre's place differs from the eye's sideways too when the pair turns "up", and that part is
		// only where the motion would have taken it; sliding the camera sideways over the tick showed as a 6x step in t28_transit7)
		Vec3 eyeFeet = PortalPair.point(a, b, e1).subtract(up);
		Vec3 centreFeet = PortalPair.point(a, b, e1.subtract(up).add(half)).subtract(half);
		// Out of a floor portal the body stays under the eye (it rises out: the floor exit's minimum speed is for the deeper feet,
		// exitSpeed); out of a wall or a tilted portal it comes out at Chell's height, so a fling flies as hers does.
		boolean floorExit = b[1].y > 0.5;
		Vec3 feet = floorExit ? eyeFeet : eyeFeet.add(0, Math.max(0.0, centreFeet.y - eyeFeet.y), 0);
		Vec3 vIn = player.getDeltaMovement();
		// Portal 2's gravity fix-up: the gravity of the part of the tick after the crossing is world-down, not turned with the pair
		double g = PlayerFling.flinging(player) ? PlayerFling.GRAVITY : player.getGravity();
		Vec3 gamma = new Vec3(0, g * (1.0 - t), 0);
		Vec3 v = PortalPair.dir(a, b, vIn.add(gamma)).subtract(gamma);
		v = exitSpeed(b, v, feet.add(half), floorExit ? eyeH : half.y);
		// the look, turned by the pair (the yaw nearest the old one: no 360 jump)
		float yaw0 = player.getYRot(), pitch0 = player.getXRot();
		float[] q = quat(a, b);
		float[] yp = turn(q, yaw0, pitch0);

		Vec3 entryFeet = player.position();
		player.setPos(feet.x, feet.y, feet.z);
		player.setYRot(yp[0]);
		player.setXRot(yp[1]);
		// Everything that lags or remembers the look moves with it (bridge #30; the field list is Immersive Portals', which snaps the
		// sway to the new look - TransformationManager.managePlayerRotationAndChangeGravity - where this keeps it): the first-person
		// hand turns by (look - bob) * 0.1, bob chasing the look at half the gap per tick, so a bob left in the old frame swung the
		// hand through the whole turn. Shifting bob (and its previous tick) by the turn keeps (look - bob) exactly as it was; the head
		// and body turn the same way, with their offsets from the look and their previous-tick values, for the third-person body.
		float dYaw = yp[0] - yaw0, dPitch = yp[1] - pitch0;
		swayBefore(player);
		if (System.getenv("PCB_NO_SWAY_FIX") == null) {  // (A/B for the tests: the env var leaves the bob in the old frame)
			player.yBob += dYaw;
			player.yBobO += dYaw;
			player.xBob += dPitch;
			player.xBobO += dPitch;
		}
		player.setYHeadRot(player.getYHeadRot() + dYaw);
		player.yHeadRotO += dYaw;
		player.setYBodyRot(player.yBodyRot + dYaw);
		player.yBodyRotO += dYaw;
		player.setOldPosAndRot(prevFeet, yp[0], yp[1]);
		swayAfter(player, dYaw, dPitch);
		player.setDeltaMovement(v);
		player.resetFallDistance();
		Vec3 freed = freeSpace(player, b);
		// on the ground at the exit (walked through a wall portal) or in the air (a fall, a fling): as at the exit, not the entry
		boolean ground = v.y <= 0 && !player.level().noCollision(player, player.getBoundingBox().move(0, -0.04, 0));
		player.setOnGround(ground);
		boolean ready = exitReady(minecraft, player.position());

		seq++;
		mc.transitSeq = seq;
		mc.transitPortals = in | (in ^ 1) << 8;
		System.arraycopy(q, 0, mc.transitQ, 0, 4);
		// off = b.centre - R a.centre
		Vec3 ra = rotate(q, a[0]);
		mc.transitOff[0] = b[0].x - ra.x;
		mc.transitOff[1] = b[0].y - ra.y;
		mc.transitOff[2] = b[0].z - ra.z;
		mc.transitFrac = (float) t;
		PENDING_SEQ[seq & 7] = seq;
		PENDING_Q[seq & 7] = q;
		PlayerFling.begin(player, seq);
		if (ground && v.x * v.x + v.z * v.z <= PlayerFling.SLIDE_END * PlayerFling.SLIDE_END) {
			PlayerFling.end();  // (walked out: no fling; the transit's own fall immunity still covers the server's next packets)
		}
		// the server's player follows with the first movement packet from the exit side (PlayerTransitServerMixin)
		PlayerFling.serverTransitPending(seq, entryFeet, player.position(), v, yp[0], yp[1]);
		if (!ready) {
			dev.skycraft.client.SkyClient.holdAfterTransit(player.position(), v);
			if (heldLogs++ < 40) {
				SkyCraft.LOG.info("Portalcraft: BUG: player transit {}: the exit's collision is not in Minecraft yet: held at {} until it is", seq, fmt(player.position()));
			}
		}
		if (logs < 200) {
			logs++;
			SkyCraft.LOG.info("Portalcraft: player transit {}: portal {} -> {} at {} of the tick: eye {} -> {}, feet {}{}, speed {} in {} out (u/s), velocity {} -> {}, look {} {} -> {} {}",
				seq, in, in ^ 1, String.format("%.2f", t), fmt(e1), fmt(PortalPair.point(a, b, e1)), fmt(player.position()), freed != null ? " (freed " + fmt(freed) + ")" : "",
				String.format("%.0f", vIn.length() / PlayerFling.U_PER_S), String.format("%.0f", v.length() / PlayerFling.U_PER_S), fmt(vIn), fmt(v), String.format("%.1f", yaw0),
				String.format("%.1f", pitch0), String.format("%.1f", yp[0]), String.format("%.1f", yp[1]));
		}
	}

	// ---- sway log (bridge #30): the hand's rotation (look - bob) at the transit and per frame around it -------------------------

	private static float swayX, swayY;
	private static int swayLogs;
	private static final String[] FRAMES = new String[16];
	private static int frameN, afterFrames;

	private static void swayBefore(LocalPlayer p) {
		swayX = p.getXRot() - p.xBob;
		swayY = p.getYRot() - p.yBob;
	}

	private static void swayAfter(LocalPlayer p, float dYaw, float dPitch) {
		if (swayLogs++ < 40) {
			SkyCraft.LOG.info("Portalcraft: hand sway at transit {}: turn ({} yaw, {} pitch), (look - bob) with the bob left in the old frame ({} {}), now ({} {}), bob ({} {}) look ({} {})",
				seq + 1, String.format("%.1f", dYaw), String.format("%.1f", dPitch), String.format("%.3f", swayX), String.format("%.3f", swayY),
				String.format("%.3f", p.getXRot() - p.xBob), String.format("%.3f", p.getYRot() - p.yBob), String.format("%.1f", p.xBob), String.format("%.1f", p.yBob),
				String.format("%.1f", p.getXRot()), String.format("%.1f", p.getYRot()));
		}
		// the frames before the transit that were logged, and the next ones
		for (int i = 0; i < FRAMES.length; i++) {
			String f = FRAMES[(frameN + i) % FRAMES.length];
			if (f != null && swayLogs <= 6) {
				SkyCraft.LOG.info("Portalcraft: hand sway frame before transit {}: {}", seq + 1, f);
			}
		}
		afterFrames = 40;
	}

	/** Per rendered frame (GameRendererViewMixin): what ItemInHandRenderer turns the arm by, (view - bob) * 0.1 per axis. */
	public static void swayFrame(float viewX, float viewY, float bobX, float bobY, float walkBob, float walkDist, float hurt, float handHeight, float swing, float avatarX, float hudFov) {
		String f = String.format("view (%.2f %.2f) bob (%.2f %.2f) sway (%.3f %.3f) | walkBob %.4f walkDist %.3f hurt %.2f handHeight %.2f swing %.2f avatarXRot %.1f hudFov %.1f", viewX, viewY, bobX, bobY,
			viewX - bobX, viewY - bobY, walkBob, walkDist, hurt, handHeight, swing, avatarX, hudFov);
		FRAMES[frameN % FRAMES.length] = f;
		frameN++;
		if (afterFrames > 0 && swayLogs <= 6) {
			afterFrames--;
			SkyCraft.LOG.info("Portalcraft: hand sway frame after transit {}: {}", seq, f);
		}
	}

	/** Portal 2's exit speed rules for a player (portal-transit.md 1.4), in blocks per tick. */
	private static Vec3 exitSpeed(Vec3[] b, Vec3 v, Vec3 centre, double feetDepth) {
		Vec3 n = b[1];
		double minOut = -1e9;
		if (n.y > 0.8660254) {
			// 300 u/s for Chell, whose hull centre is at the plane (feet 36 u under it); the Minecraft player comes out with its eye
			// at the plane (feet eyeH under it): as fast as reaches the same height above the floor portal
			double v0 = 300.0 * PlayerFling.U_PER_S;
			minOut = Math.sqrt(v0 * v0 + 2.0 * PlayerFling.GRAVITY * Math.max(0.0, feetDepth - 0.9));
		} else if (n.y > 0.5) {
			double arc = arcSpeed(b, centre);
			if (arc > 0) {
				minOut = Math.min(arc, 300.0 * PlayerFling.U_PER_S);
			}
		}
		double out = v.dot(n);
		if (out < minOut) {
			v = v.add(n.scale(minOut - out));
		} else if (v.length() > 1000.0 * PlayerFling.U_PER_S) {
			v = v.scale(1000.0 * PlayerFling.U_PER_S / v.length());
		}
		double m = PlayerFling.MAX_AXIS;
		return new Vec3(Math.max(-m, Math.min(m, v.x)), Math.max(-m, Math.min(m, v.y)), Math.max(-m, Math.min(m, v.z)));
	}

	/** Portal 2's launch speed out of a tilted portal that reaches its bottom edge (+ half a hull) on a ballistic arc. */
	private static double arcSpeed(Vec3[] b, Vec3 centre) {
		Vec3 t = b[0].subtract(b[2].scale(PortalPair.halfH(b))).subtract(centre).add(0, 0.9, 0);
		t = t.subtract(b[3].scale(t.dot(b[3])));
		double x = Math.sqrt(t.x * t.x + t.z * t.z), y = t.y;
		double fh = Math.sqrt(b[1].x * b[1].x + b[1].z * b[1].z), fz = b[1].y;
		double a = -2.0 * fz * (x * fh - y * fz);
		if (a >= 0) {
			return -1;
		}
		double v2 = PlayerFling.GRAVITY * x * x / -a;
		return v2 > 0 ? Math.sqrt(v2) : -1;
	}

	/**
	 * Portal 2's own exit check (portal-transit.md 1.5): a body that starts in something solid at the exit is moved to the nearest
	 * free spot in front of the exit (up first, then out along the exit's normal). Returns the move, or null.
	 */
	private static Vec3 freeSpace(LocalPlayer player, Vec3[] b) {
		var level = player.level();
		AABB box = player.getBoundingBox();
		if (level.noCollision(player, box)) {
			return null;
		}
		if (freeLogs++ < 40) {
			for (var shape : level.getBlockCollisions(player, box)) {
				if (!shape.isEmpty() && shape.bounds().intersects(box)) {
					SkyCraft.LOG.info("Portalcraft: player transit {}: the exit's box {} overlaps {}", seq + 1, box, shape.bounds());
					break;
				}
			}
		}
		Vec3 n = b[1];
		// (1) a small step up or out (the eye's height at the exit put the feet a little into the exit's floor)
		for (int k = 1; k <= 8; k++) {
			double s = k * 0.05;
			for (Vec3 d : new Vec3[] { new Vec3(0, s, 0), n.scale(s), n.scale(s).add(0, s, 0) }) {
				if (level.noCollision(player, box.move(d))) {
					// (the last tick's end stays where the pair put it: the step is spread over this tick, no pop in the view)
					player.setPos(player.getX() + d.x, player.getY() + d.y, player.getZ() + d.z);
					return d;
				}
			}
		}
		// (2) Portal 2's own retry: the hull's centre on the exit's axis, at the same distance from its plane (a body that came out
		// at the edge of the opening, its feet a block into the floor under a wall portal)
		Vec3 c = box.getCenter();
		double along = c.subtract(b[0]).dot(n);
		Vec3 onAxis = b[0].add(n.scale(Math.max(along, 0.0) + 0.02));
		for (int k = 0; k <= 8; k++) {
			Vec3 d = onAxis.subtract(c).add(n.scale(k * 0.05));
			if (level.noCollision(player, box.move(d))) {
				player.setPos(player.getX() + d.x, player.getY() + d.y, player.getZ() + d.z);
				return d;
			}
		}
		if (freeLogs++ < 40) {
			SkyCraft.LOG.info("Portalcraft: BUG: player transit {}: no free spot at the exit {}", seq + 1, box);
		}
		return null;
	}

	/** The exit's collision is in Minecraft: the host's export there, or Minecraft's own terrain under it (a loaded chunk). */
	private static boolean exitReady(Minecraft minecraft, Vec3 p) {
		int bx = (int) Math.floor(p.x), by = (int) Math.floor(p.y), bz = (int) Math.floor(p.z);
		if (SkyCollision.isKnown(bx, by, bz) && SkyCollision.isKnown(bx, by - 1, bz)) {
			return true;
		}
		var level = minecraft.level;
		if (level == null || !level.hasChunk(bx >> 4, bz >> 4)) {
			return false;
		}
		for (int d = 0; d <= 64; d++) {
			BlockPos q = new BlockPos(bx, by - d, bz);
			if (q.getY() < level.getMinY()) {
				break;
			}
			if (!level.getBlockState(q).isAir()) {
				return true;
			}
		}
		return false;
	}

	/**
	 * The host's look (SkyState yaw/pitch) as the player's: turned by every transit the host has not yet turned its own look for
	 * (transitAck). Returns {yaw, pitch}.
	 */
	public static float[] look(int ack, float yaw, float pitch) {
		if (ack == seq || seq == 0) {
			return new float[] { yaw, pitch };
		}
		int from = Math.max(ack, seq - 7);
		for (int s = from + 1; s <= seq; s++) {
			if (PENDING_SEQ[s & 7] == s && PENDING_Q[s & 7] != null) {
				float[] yp = turn(PENDING_Q[s & 7], yaw, pitch);
				yaw = yp[0];
				pitch = yp[1];
			}
		}
		return new float[] { yaw, pitch };
	}

	// ---- the pair's rotation as a quaternion (MC axes) --------------------------------------------------------------------

	/** The rotation part of PortalPair.dir(a, b, .) as a unit quaternion {x, y, z, w}. */
	static float[] quat(Vec3[] a, Vec3[] b) {
		Vec3 x = PortalPair.dir(a, b, new Vec3(1, 0, 0)), y = PortalPair.dir(a, b, new Vec3(0, 1, 0)), z = PortalPair.dir(a, b, new Vec3(0, 0, 1));
		// columns of R are the images of the axes
		double m00 = x.x, m10 = x.y, m20 = x.z, m01 = y.x, m11 = y.y, m21 = y.z, m02 = z.x, m12 = z.y, m22 = z.z;
		double tr = m00 + m11 + m22, qw, qx, qy, qz;
		if (tr > 0) {
			double s = Math.sqrt(tr + 1.0) * 2;
			qw = 0.25 * s;
			qx = (m21 - m12) / s;
			qy = (m02 - m20) / s;
			qz = (m10 - m01) / s;
		} else if (m00 > m11 && m00 > m22) {
			double s = Math.sqrt(1.0 + m00 - m11 - m22) * 2;
			qw = (m21 - m12) / s;
			qx = 0.25 * s;
			qy = (m01 + m10) / s;
			qz = (m02 + m20) / s;
		} else if (m11 > m22) {
			double s = Math.sqrt(1.0 + m11 - m00 - m22) * 2;
			qw = (m02 - m20) / s;
			qx = (m01 + m10) / s;
			qy = 0.25 * s;
			qz = (m12 + m21) / s;
		} else {
			double s = Math.sqrt(1.0 + m22 - m00 - m11) * 2;
			qw = (m10 - m01) / s;
			qx = (m02 + m20) / s;
			qy = (m12 + m21) / s;
			qz = 0.25 * s;
		}
		double l = Math.sqrt(qx * qx + qy * qy + qz * qz + qw * qw);
		return new float[] { (float) (qx / l), (float) (qy / l), (float) (qz / l), (float) (qw / l) };
	}

	static Vec3 rotate(float[] q, Vec3 v) {
		double x = q[0], y = q[1], z = q[2], w = q[3];
		// v + 2w (q x v) + 2 q x (q x v)
		double cx = y * v.z - z * v.y, cy = z * v.x - x * v.z, cz = x * v.y - y * v.x;
		double ccx = y * cz - z * cy, ccy = z * cx - x * cz, ccz = x * cy - y * cx;
		return new Vec3(v.x + 2 * (w * cx + ccx), v.y + 2 * (w * cy + ccy), v.z + 2 * (w * cz + ccz));
	}

	/** Minecraft yaw/pitch turned by q; the yaw nearest the given one. Looking straight up or down, the yaw comes from "up". */
	static float[] turn(float[] q, float yaw, float pitch) {
		double yr = Math.toRadians(yaw), pr = Math.toRadians(pitch);
		Vec3 f = new Vec3(-Math.sin(yr) * Math.cos(pr), -Math.sin(pr), Math.cos(yr) * Math.cos(pr));
		Vec3 u = new Vec3(-Math.sin(yr) * Math.sin(pr), Math.cos(pr), Math.cos(yr) * Math.sin(pr));  // the camera's up (the look at pitch - 90)
		Vec3 f2 = rotate(q, f), u2 = rotate(q, u);
		float p2 = (float) Math.toDegrees(-Math.asin(Math.max(-1.0, Math.min(1.0, f2.y))));
		double hx = f2.x, hz = f2.z;
		if (hx * hx + hz * hz < 1e-6) {
			// straight down (pitch 90): the camera's up is the look's horizontal direction; straight up: against it
			double s = f2.y > 0 ? -1.0 : 1.0;
			hx = u2.x * s;
			hz = u2.z * s;
		}
		float y2 = (float) Math.toDegrees(Math.atan2(-hx, hz));
		y2 = yaw + (float) Math.IEEEremainder(y2 - yaw, 360.0);
		return new float[] { y2, Math.max(-90f, Math.min(90f, p2)) };
	}

	private static String fmt(Vec3 v) {
		return String.format("(%.3f %.3f %.3f)", v.x, v.y, v.z);
	}
}
