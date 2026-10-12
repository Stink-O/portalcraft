package dev.skycraft.portalcraft;

import net.minecraft.world.entity.Entity;
import net.minecraft.world.entity.LivingEntity;
import net.minecraft.world.phys.Vec3;

/**
 * Portalcraft (bridge #28): the Minecraft player's flight after a portal transit is Portal 2's fling, as Chell's is. From the
 * transit until it lands (ground, water, a ladder, flying, elytra, a vehicle), the client's player keeps its momentum the way
 * Portal 2 keeps a player's (research/specs/portal-movement.md 5 and 7): gravity 600 u/s^2, no air drag, Portal 2's air control
 * (sv_paintairacceleration 5, at most 60 u/s along the wish direction), every axis capped at sv_maxvelocity 3500 u/s. Minecraft's
 * own air drag (x 0.91 a tick) and gravity (0.08) would end a 1000 u/s fling within 14 blocks, where Chell flies on.
 * <p>
 * Fall damage: Portal 2 never hurts Chell on a landing (long-fall boots), so the landing that ends a fling, and the transit itself,
 * never hurt the Minecraft player either. The server applies that (PlayerFlingFallMixin) until it has seen the client land after the
 * latest transit, so a movement packet from before the transit (or the transit's own position jump) cannot slip a fall through.
 * Single player: the client's and the server's threads share this state.
 */
public final class PlayerFling {
	/** Portal 2 units in Minecraft blocks per tick: 1 block = 40 u, 20 ticks a second. */
	public static final double U_PER_S = 1.0 / (40.0 * 20.0);
	/** sv_gravity 600 u/s^2, in blocks per tick per tick. */
	public static final double GRAVITY = 600.0 / (40.0 * 400.0);
	public static final double MAX_AXIS = 3500.0 * U_PER_S;
	private static final double AIR_CAP = 60.0 * U_PER_S, MAX_SPEED = 175.0 * U_PER_S, AIR_ACCEL = 5.0, ACCEL = 10.0, FRICTION = 4.0, STOP_SPEED = 100.0 * U_PER_S;
	/** A fling on the ground ends (Minecraft's own ground rules again) once it is no faster than a sprint. */
	public static final double SLIDE_END = 0.28;

	/** Bridge #32: an excursion funnel carries the player (HostMechanics): no gravity and no air control, the host sets its velocity. */
	public static volatile boolean beam;
	private static int launchCount;

	/** Bridge #32: Portal 2's gravity for a flinging player (none inside a funnel). */
	public static double gravity() {
		return beam ? 0.0 : GRAVITY;
	}

	/** Bridge #32: a fling that starts from a faith plate or a funnel (not a portal): the same physics and the same fall immunity. */
	public static void beginLaunch(Entity player) {
		begin(player, (1 << 20) + ++launchCount);
	}

	/** The client's player while it flings (null otherwise). */
	public static volatile Entity flinger;
	/** Transits so far, the transit the client last landed after, and the one the server last saw a landing for. */
	public static volatile int transitSeq, landedSeq, serverSeq;
	public static volatile long transitNs;

	private PlayerFling() {
	}

	// ---- the server's copy of the player through the transit ----------------------------------------------------------------
	// The client transits at the end of its tick, after it has sent that tick's position (still on the entry side). Snapping the
	// server's player to the exit at once raced that packet: processed after the snap, it moved the server's player back to the
	// entry side, and the next one (at the exit) was refused as "moved wrongly" into geometry: the client was pulled back into the
	// entry's hole. Instead the server's player is moved to the exit by the first movement packet from the exit side, and packets
	// still from the entry side are dropped (PlayerTransitServerMixin).
	private static volatile int pendingSeq;
	private static volatile Vec3 pendingEntry, pendingExit, pendingVel;
	private static volatile float pendingYaw, pendingPitch;
	private static volatile long pendingNs;
	private static int dropped;

	public static void serverTransitPending(int seq, Vec3 entry, Vec3 exit, Vec3 vel, float yaw, float pitch) {
		pendingEntry = entry;
		pendingExit = exit;
		pendingVel = vel;
		pendingYaw = yaw;
		pendingPitch = pitch;
		pendingNs = System.nanoTime();
		pendingSeq = seq;
	}

	/** Server thread, a movement packet with a position: false = drop it (it is from before the transit). */
	public static boolean serverMove(net.minecraft.server.level.ServerPlayer sp, Runnable resetPosition, double x, double y, double z) {
		int seq = pendingSeq;
		if (seq == 0) {
			return true;
		}
		Vec3 entry = pendingEntry, exit = pendingExit, t = new Vec3(x, y, z);
		boolean exitSide = t.distanceToSqr(exit) <= t.distanceToSqr(entry) || System.nanoTime() - pendingNs > 1_000_000_000L;
		if (!exitSide) {
			if (dropped++ < 40) {
				dev.skycraft.SkyCraft.LOG.info("Portalcraft: player transit {}: a movement packet from before it dropped (server)", seq);
			}
			return false;
		}
		pendingSeq = 0;
		sp.absSnapTo(exit.x, exit.y, exit.z, pendingYaw, pendingPitch);
		sp.setDeltaMovement(pendingVel);
		sp.resetFallDistance();
		resetPosition.run();
		return true;
	}

	public static boolean flinging(Object e) {
		return e != null && e == flinger;
	}

	/** A transit happened: the fling starts, and fall damage is off until the client and then the server have seen a landing. */
	public static void begin(Entity player, int seq) {
		transitSeq = seq;
		transitNs = System.nanoTime();
		flinger = player;
	}

	/** The client's player landed (or the fling ended any other way). */
	public static void end() {
		flinger = null;
		landedSeq = transitSeq;
	}

	/** The server's copy of the player is immune to fall damage now. */
	public static boolean serverImmune() {
		return serverSeq != transitSeq || System.nanoTime() - transitNs < 300_000_000L;
	}

	/**
	 * Portal 2's move for a flinging player, before Minecraft's own travel (which then moves it with no friction or drag, Portal 2's
	 * gravity and no input of its own, PlayerFlingMixin), over the three Portal 2 ticks of one Minecraft tick: in the air
	 * AirAccelerate (at most 60 u/s along the wish direction), on the ground Portal 2's friction (sv_friction 4, stopspeed 100) and
	 * Accelerate (sv_accelerate 10 up to the walking speed): a fling that lands slides on as Chell does.
	 */
	public static void airMove(LivingEntity e, Vec3 input) {
		if (beam) {
			return;  // (inside a funnel the host sets the velocity)
		}
		Vec3 v = e.getDeltaMovement();
		boolean ground = e.onGround();
		double ix = input.x, iz = input.z;
		double len = Math.sqrt(ix * ix + iz * iz);
		double wx = 0, wz = 0, wishSpeed = 0;
		if (len > 1e-4) {
			double yr = Math.toRadians(e.getYRot()), s = Math.sin(yr), c = Math.cos(yr);
			// (Minecraft's moveRelative: input x is left, z forward, turned by the yaw)
			wx = (ix * c - iz * s) / len;
			wz = (iz * c + ix * s) / len;
			wishSpeed = Math.min(1.0, len) * MAX_SPEED;
		}
		for (int k = 0; k < 3; k++) {
			if (ground) {
				double sp = Math.sqrt(v.x * v.x + v.z * v.z);
				if (sp > 1e-6) {
					double drop = Math.max(sp, STOP_SPEED) * FRICTION / 60.0, ns = Math.max(0.0, sp - drop) / sp;
					v = new Vec3(v.x * ns, v.y, v.z * ns);
				}
			}
			if (wishSpeed > 0) {
				double cap = ground ? wishSpeed : Math.min(wishSpeed, AIR_CAP);
				double add = cap - (v.x * wx + v.z * wz);
				if (add > 0) {
					double acc = Math.min(add, wishSpeed * (ground ? ACCEL : AIR_ACCEL) / 60.0);
					v = new Vec3(v.x + wx * acc, v.y, v.z + wz * acc);
				}
			}
		}
		v = new Vec3(clamp(v.x), clamp(v.y), clamp(v.z));
		e.setDeltaMovement(v);
	}

	private static double clamp(double a) {
		return Math.max(-MAX_AXIS, Math.min(MAX_AXIS, a));
	}
}
