package dev.skycraft.portalcraft;

import dev.skycraft.SkyCraft;
import dev.skycraft.world.PortalPair;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.entity.Mob;
import net.minecraft.world.entity.decoration.HangingEntity;
import net.minecraft.world.entity.player.Player;
import net.minecraft.world.phys.AABB;
import net.minecraft.world.phys.Vec3;

/**
 * Portalcraft (bridge #22): Minecraft's mobs, dropped items, XP orbs, projectiles and falling blocks go through the host's
 * linked portal pair (PortalPair), as Portal 2 carries its props: an entity whose centre crosses a portal's plane inside
 * the opening during a tick is put at the same spot relative to the partner (half a turn about the up axis), with its
 * velocity and facing turned the same way. A mob pushed into the cliff portal comes out of the chamber's portal and back.
 * The player goes through in PlayerTransit (client side, bridge #28), on its eye. Blocks behind a portal are
 * carved out of these entities' collision (BlockCollisionsMixin) so they can reach the plane.
 */
public final class EntityTransit {
	private static final Map<Integer, Long> COOLDOWN = new HashMap<>();
	// (bridge #48) where each entity near a portal was at the end of the last tick. An arrow's own "previous position" is its end-of-move spot
	// (it moves, then ticks its parents), so a crossing could never be seen from it.
	private static Map<Integer, Vec3> prevCentre = new HashMap<>();
	private static final double NEAR = 8.0;
	private static int transits, logs, nearLogs;

	private EntityTransit() {
	}

	public static int transits() {
		return transits;
	}

	public static void tick(ServerLevel level) {
		if (!PortalPair.anyLinked()) {
			if (!prevCentre.isEmpty()) {
				prevCentre = new HashMap<>();
			}
			return;
		}
		Vec3[][] pose = new Vec3[PortalPair.SLOTS][];
		for (int i = 0; i < PortalPair.SLOTS; i++) {
			pose[i] = PortalPair.pose(i);
		}
		long now = level.getGameTime();
		Map<Integer, Vec3> nextCentre = new HashMap<>();
		COOLDOWN.values().removeIf(t -> now - t > 40);
		for (int i = 0; i < PortalPair.SLOTS; i++) {
			if (pose[i] == null || pose[i ^ 1] == null) {
				continue;  // (bridge #32: every slot whose partner, the other colour of its linkage group, is live)
			}
			Vec3 c = pose[i][0];
			AABB near = new AABB(c, c).inflate(NEAR);
			List<Entity> list = level.getEntities((Entity) null, near, e -> !(e instanceof Player) && !(e instanceof HangingEntity)
				&& !(e instanceof dev.skycraft.combat.SkyrimActorEntity) && !e.isPassenger() && !e.isVehicle() && e.isAlive());
			for (Entity e : list) {
				Long last = COOLDOWN.get(e.getId());
				if (last != null && now - last < 5) {
					nextCentre.put(e.getId(), e.getBoundingBox().getCenter());
					continue;
				}
				AABB bb = e.getBoundingBox();
				Vec3 cen = bb.getCenter();
				Vec3 before = prevCentre.get(e.getId());
				Vec3 old = before != null ? before : cen.add(e.xo - e.getX(), e.yo - e.getY(), e.zo - e.getZ());
				nextCentre.putIfAbsent(e.getId(), cen);
				double dNow = PortalPair.depth(pose[i], cen), dOld = PortalPair.depth(pose[i], old);
				if (Math.abs(dNow) < 1.5 && now % 5 == 0 && nearLogs < 200) {
					nearLogs++;
					SkyCraft.LOG.info("Portalcraft: near portal {}: {} centre {} depth {} (last tick {}), in hole {}", i, e.getType().toShortString(), cen,
						String.format("%.3f", dNow), String.format("%.3f", dOld), PortalPair.inHole(pose[i], cen));
				}
				// (bridge #48: a fast arrow ends its tick as far past the plane as it flew; its clip went through the hole, deepened to its reach)
				if (!(dOld >= 0.0 && dNow < 0.0 && dNow > -Math.max(2.0, cen.distanceTo(old) + 0.5))) {
					continue;
				}
				// where the centre crossed the plane: inside the opening
				double t = dOld / (dOld - dNow);
				Vec3 hit = old.add(cen.subtract(old).scale(t));
				// (the carved hole is the opening's rectangle: whatever got its centre that far in goes through, or it stuck in a
				// corner of the hole, half in the cliff: b22_b4)
				if (!PortalPair.inHole(pose[i], hit)) {
					continue;
				}
				carry(e, pose[i], pose[i ^ 1], cen);
				nextCentre.put(e.getId(), e.getBoundingBox().getCenter());
				COOLDOWN.put(e.getId(), now);
				transits++;
				if (logs++ < 40) {
					SkyCraft.LOG.info("Portalcraft: {} went through portal {} -> {}: now at {} moving {}", e.getType().toShortString(), i, i ^ 1, e.position(), e.getDeltaMovement());
				}
			}
		}
		prevCentre = nextCentre;
	}

	private static void carry(Entity e, Vec3[] a, Vec3[] b, Vec3 centre) {
		Vec3 feetOff = e.position().subtract(centre);
		Vec3 c2 = PortalPair.point(a, b, centre);
		// the body stays upright (Portal 2 keeps its props' orientation through the pair; Minecraft's entities are always
		// upright): the feet hang below the carried centre
		Vec3 feet = c2.add(feetOff);
		Vec3 v = PortalPair.dir(a, b, e.getDeltaMovement());
		float yaw = e.getYRot();
		double yr = Math.toRadians(yaw);
		Vec3 look = PortalPair.dir(a, b, new Vec3(-Math.sin(yr), 0, Math.cos(yr)));
		if (look.horizontalDistanceSqr() > 1e-4) {
			yaw = (float) Math.toDegrees(Math.atan2(-look.x, look.z));
		}
		e.snapTo(feet.x, feet.y, feet.z, yaw, e.getXRot());
		e.setYHeadRot(yaw);
		e.setYBodyRot(yaw);
		e.setOldPosAndRot();
		e.setDeltaMovement(v);
		e.resetFallDistance();
		e.needsSync = true;  // the new velocity reaches the client this tick
		if (e instanceof Mob m) {
			m.getNavigation().stop();
		}
	}
}
