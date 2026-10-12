package dev.skycraft.portalcraft;

import net.minecraft.world.entity.player.Player;
import net.minecraft.world.phys.Vec3;

/**
 * Portalcraft (bridge #32): Portal 2's chamber mechanics acting on the Minecraft player. The host (src/bridge/mcmechanics.cpp) sees the
 * Minecraft player enter a faith plate's volume or a funnel and sends what Portal 2 would do to a player there, as input type
 * {@link #IN_MECH} (a, b, c = a velocity in blocks per tick x 10000, Minecraft axes):
 * <ul>
 * <li>{@link #LAUNCH}: a faith plate's launch. The player's velocity becomes it, off the ground, and the flight is Portal 2's fling
 * (PlayerFling: gravity 600 u/s^2, no air drag, Portal 2's air control; no fall damage for the landing), as after a portal transit.</li>
 * <li>{@link #BEAM}: inside an excursion funnel. The host sends the beam's velocity (linearForce along it, pulled to its axis) about 20
 * times a second; for 250 ms after each the player has that velocity at every tick start, with no gravity. When the updates stop (or
 * {@link #BEAM_END}) the player leaves the funnel with the velocity it had, in Portal 2's fling.</li>
 * <li>{@link #SPEED} (bridge #37): speed gel. The host sends the player's ground speed limit as a multiple of the walking speed (a = the
 * multiple x 10000, about 20 times a second); for 250 ms after each, a player walking on the ground has its horizontal velocity scaled so
 * that Minecraft's own walking (friction 0.546, the walking acceleration) settles at that multiple of its normal speed.</li>
 * <li>{@link #PUSH} (bridge #67): a trigger_push the player stands in. The host sends its push (blocks per tick) about 20 times a second, zero when
 * the player leaves it; for 250 ms after each, the player is displaced by it every tick (Source's base velocity: it moves the player without
 * changing its velocity, so it ends with the volume). A push upward takes a grounded player off the ground.</li>
 * </ul>
 * Older hosts never send the type; older mods ignore it.
 */
public final class HostMechanics {
	public static final int IN_MECH = 16;
	public static final int LAUNCH = 1, BEAM = 2, BEAM_END = 3, SPEED = 4, PUSH = 5;
	private static final long BEAM_HOLD_NS = 250_000_000L;

	private static volatile Vec3 launch;
	private static volatile Vec3 beamVel;
	private static volatile long beamUntilNs;
	private static volatile Vec3 pushVel;
	private static volatile long pushUntilNs;
	private static volatile double speedMul = 1.0;
	private static volatile long speedUntilNs;
	private static int logs, pushLogs;
	private static boolean wasPushing;

	private HostMechanics() {
	}

	/** Input thread: a mechanic message from the host. */
	public static void onInput(int code, int a, int b, int c) {
		Vec3 v = new Vec3(a / 10000.0, b / 10000.0, c / 10000.0);
		switch (code) {
			case LAUNCH -> launch = v;
			case BEAM -> {
				beamVel = v;
				beamUntilNs = System.nanoTime() + BEAM_HOLD_NS;
			}
			case BEAM_END -> beamUntilNs = 0L;
			case PUSH -> {
				// (bridge #67) a trigger_push: zero ends it
				pushVel = (a == 0 && b == 0 && c == 0) ? null : v;
				pushUntilNs = System.nanoTime() + BEAM_HOLD_NS;
			}
			case SPEED -> {
				speedMul = Math.max(1.0, Math.min(5.0, a / 10000.0));
				speedUntilNs = System.nanoTime() + BEAM_HOLD_NS;
			}
			default -> {
			}
		}
	}

	/** Start of the local player's tick (LocalPlayerMoverMixin), before its move. */
	public static void tickStart(Player player) {
		Vec3 l = launch;
		if (l != null) {
			launch = null;
			player.setDeltaMovement(l);
			if (l.y > 0.01) {
				player.setOnGround(false);
			}
			player.resetFallDistance();
			PlayerFling.beginLaunch(player);
			if (logs++ < 60) {
				dev.skycraft.SkyCraft.LOG.info("Portalcraft: faith plate launch: velocity {} u/s ({} blocks/tick)", String.format("%.0f %.0f %.0f", l.x / PlayerFling.U_PER_S, l.y / PlayerFling.U_PER_S, l.z / PlayerFling.U_PER_S), l);
			}
		}
		Vec3 pv = pushVel;
		boolean pushing = pv != null && System.nanoTime() < pushUntilNs;
		if (pushing) {
			if (pv.y > 0.01) {
				player.setOnGround(false);  // Source lifts a grounded player the push has an upward part for
			}
			player.move(net.minecraft.world.entity.MoverType.SELF, pv);
		}
		if (pushing != wasPushing) {
			wasPushing = pushing;
			if (pushLogs++ < 40) {
				dev.skycraft.SkyCraft.LOG.info("Portalcraft: trigger_push: {} (blocks/tick {})", pushing ? "the player is pushed" : "the push ended", pv);
			}
		}
		double m = speedMul;
		if (m > 1.01 && System.nanoTime() < speedUntilNs && player.onGround() && !PlayerFling.beam) {
			// ground speed with gel: Minecraft's walking settles where v = (s v + a) * 0.546 (s = this scale), a = the walking acceleration; a limit of m times
			// the normal speed needs s = (1 - 0.454 / m) / 0.546 (s = 1 at m = 1)
			Vec3 d = player.getDeltaMovement();
			double hv = Math.sqrt(d.x * d.x + d.z * d.z);
			if (hv > 0.04) {
				double s = (1.0 - 0.454 / m) / 0.546;
				player.setDeltaMovement(d.x * s, d.y, d.z * s);
			}
		}
		Vec3 bv = beamVel;
		boolean inBeam = bv != null && System.nanoTime() < beamUntilNs;
		if (inBeam) {
			if (!PlayerFling.beam) {
				PlayerFling.beam = true;
				PlayerFling.beginLaunch(player);
				if (logs++ < 60) {
					dev.skycraft.SkyCraft.LOG.info("Portalcraft: funnel: the player is carried, velocity {} blocks/tick", bv);
				}
			}
			PlayerFling.flinger = player;
			player.setDeltaMovement(bv);
			player.setOnGround(false);
			player.resetFallDistance();
		} else if (PlayerFling.beam) {
			PlayerFling.beam = false;  // out of the funnel: the fling goes on with the velocity it has
			if (logs++ < 60) {
				dev.skycraft.SkyCraft.LOG.info("Portalcraft: funnel: the player left it with {}", player.getDeltaMovement());
			}
		}
	}
}
