package dev.skycraft.portalcraft;

import dev.skycraft.SkyCraft;
import dev.skycraft.world.SkyCollision;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import net.minecraft.core.BlockPos;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.entity.LivingEntity;
import net.minecraft.world.entity.player.Player;
import net.minecraft.world.phys.AABB;
import net.minecraft.world.phys.Vec3;

/**
 * Portalcraft (bridge #19, Owner B): Minecraft mobs and things living in Portal 2's chambers.
 *
 * <ul>
 * <li><b>Host light</b> (kInHostLight): Portal 2's light, as a Minecraft light level, on the cells above blocks placed in
 * the chambers. Those cells are under Portal 2's roofs, so Minecraft's own sky light (the chamber band is open sky in
 * Minecraft) does not count there: a monster spawns on a placed block when both Portal 2's light and Minecraft's block
 * light are dark (MonsterDarknessMixin), as in a Minecraft cave.</li>
 * <li><b>Hazards</b> (kInHazard): Portal 2's goo and crushers (lethal volumes), lasers (segments), emancipation grids
 * (fizzlers) and other hurt volumes, as boxes; Minecraft's mobs and dropped things in them are hurt, burnt, dissolved
 * or killed by Portal 2's rules (the player is not: Portal 2 hurts the player itself, through its own health).</li>
 * <li><b>Holding still where Portal 2's collision is not known yet</b>: a mob in the chamber band whose ground has not
 * been exported does not fall through the chamber floor (it waits, as the player does, SkyClient).</li>
 * </ul>
 */
public final class HostWorld {
	// ---- the hidden player (Chell mode) -----------------------------------------------------------------------------
	private static final int[] HOST_PLAYER = new int[5];
	private static volatile long hiddenReadAt;
	private static volatile boolean hidden;

	/** The host's player is Chell (HostPlayer without kHpMcMode): the Minecraft player only follows her, unseen. */
	public static boolean playerHidden() {
		long now = System.nanoTime();
		if (now - hiddenReadAt > 50_000_000L) {
			hiddenReadAt = now;
			synchronized (HOST_PLAYER) {
				hidden = dev.skycraft.link.SkyLink.active() && dev.skycraft.link.SkyLink.readHostPlayer(HOST_PLAYER)
					&& (HOST_PLAYER[0] & dev.skycraft.link.Proto.HP_MC_MODE) == 0;
			}
		}
		return hidden;
	}

	/** The host's player is the Minecraft player (Portalcraft Minecraft mode). */
	public static boolean hostIsMinecraftPlayer() {
		if (!dev.skycraft.link.SkyLink.active()) {
			return false;
		}
		synchronized (HOST_PLAYER) {
			return dev.skycraft.link.SkyLink.readHostPlayer(HOST_PLAYER) && (HOST_PLAYER[0] & dev.skycraft.link.Proto.HP_MC_MODE) != 0;
		}
	}

	// ---- host light ----------------------------------------------------------------------------------------------
	private static final ConcurrentHashMap<Long, Integer> LIGHT = new ConcurrentHashMap<>();

	public static void hostLight(int code, int x, int y, int z) {
		if ((code & 0x100) != 0) {
			LIGHT.clear();
			return;
		}
		LIGHT.put(BlockPos.asLong(x, y, z), code & 0xF);
	}

	/** Portal 2's light level (0-15) at this cell when it is under Portal 2's roof, else -1 (Minecraft's own rules). */
	public static int hostLightAt(BlockPos pos) {
		Integer l = LIGHT.isEmpty() ? null : LIGHT.get(pos.asLong());
		return l == null ? -1 : l;
	}

	/** Portal 2's geometry (the exported collision) somewhere above this cell, within 32 blocks: a chamber ceiling. */
	public static boolean underHostRoof(BlockPos pos) {
		if (!SkyCollision.active()) {
			return false;
		}
		if (hostLightAt(pos) >= 0) {
			return true;
		}
		BlockPos.MutableBlockPos p = pos.mutable();
		for (int dy = 1; dy <= 32; dy++) {
			p.setY(pos.getY() + dy);
			if (SkyCollision.hasGeometry(p)) {
				return true;
			}
		}
		return false;
	}

	public static int hostLightCells() {
		return LIGHT.size();
	}

	// ---- hazards ---------------------------------------------------------------------------------------------------
	public static final int HAZARD_LETHAL = 1; // goo, crushers, death pits (trigger_hurt >= 100)
	public static final int HAZARD_LASER = 2;  // a laser beam: a segment
	public static final int HAZARD_FIZZLER = 3; // an emancipation grid: dissolves what is not a player
	public static final int HAZARD_HURT = 4;   // a trigger_hurt below lethal

	private record Hazard(int kind, Vec3 a, Vec3 b, long at) {
	}

	private static final Map<Integer, double[]> PENDING = new HashMap<>();
	private static final ConcurrentHashMap<Integer, Hazard> HAZARDS = new ConcurrentHashMap<>();
	private static int logs;

	/** kInHazard: code bits 0-3 kind, bit 4 max corner (the box is live), bit 5 clear all, bits 8-15 slot; MC x 1000. */
	public static synchronized void hazard(int code, double x, double y, double z) {
		if ((code & 0x20) != 0) {
			HAZARDS.clear();
			PENDING.clear();
			return;
		}
		int slot = (code >> 8) & 0xFF;
		if ((code & 0x10) == 0) {
			PENDING.put(slot, new double[] { x, y, z });
			return;
		}
		double[] a = PENDING.remove(slot);
		if (a == null) {
			return;
		}
		Hazard h = new Hazard(code & 0xF, new Vec3(a[0], a[1], a[2]), new Vec3(x, y, z), System.nanoTime());
		Hazard old = HAZARDS.put(slot, h);
		if (old == null && logs++ < 40) {
			SkyCraft.LOG.info("Portalcraft: Portal 2 hazard {} kind {} {} -> {}", slot, h.kind, h.a, h.b);
		}
	}

	public static int hazardCount() {
		return HAZARDS.size();
	}

	private static AABB box(Hazard h) {
		if (h.kind == HAZARD_LASER) {
			return new AABB(h.a, h.b).inflate(0.3);
		}
		return new AABB(h.a, h.b);
	}

	/** Distance from p to the segment a-b (blocks). */
	private static double segmentDistance(Vec3 p, Vec3 a, Vec3 b) {
		Vec3 ab = b.subtract(a);
		double len2 = ab.lengthSqr();
		double t = len2 < 1e-9 ? 0 : Math.max(0, Math.min(1, p.subtract(a).dot(ab) / len2));
		return p.distanceTo(a.add(ab.scale(t)));
	}

	private static int hurts, dissolved, kills;

	// ---- server tick -----------------------------------------------------------------------------------------------
	private static final Map<Integer, Boolean> HELD = new HashMap<>();
	private static final double BAND_MIN_Y = 320.0; // above vanilla's build height only the chambers' placed blocks exist

	public static void serverTick(ServerLevel level, List<ServerPlayer> players) {
		long now = System.nanoTime();
		HAZARDS.values().removeIf(h -> now - h.at > 3_000_000_000L);
		boolean hazards = !HAZARDS.isEmpty();
		boolean band = SkyCollision.active();
		if (!hazards && !band && HELD.isEmpty()) {
			return;
		}
		List<Entity> near = new ArrayList<>();
		for (ServerPlayer p : players) {
			level.getEntities((Entity) null, p.getBoundingBox().inflate(96.0), e -> !(e instanceof Player) && !(e instanceof dev.skycraft.combat.SkyrimActorEntity)).forEach(
				e -> {
					if (!near.contains(e)) {
						near.add(e);
					}
				});
		}
		for (Entity e : near) {
			if (band) {
				holdIfUnknown(e);
			}
			if (hazards && level.getGameTime() % 5 == 0) {
				applyHazards(level, e);
			}
		}
		HELD.keySet().removeIf(id -> level.getEntity(id) == null);
	}

	/** A mob in the chamber band over ground Portal 2 has not exported yet floats where it is (no fall). */
	private static void holdIfUnknown(Entity e) {
		if (e.getY() < BAND_MIN_Y || e.noPhysics || dev.skycraft.world.StageTerrain.inStage(e.getY())) {  // (bridge #47: the stage is Minecraft's own ground, the host exports nothing there)
			return;
		}
		int x = e.getBlockX(), y = e.getBlockY(), z = e.getBlockZ();
		boolean known = SkyCollision.isKnown(x, y, z) && SkyCollision.isKnown(x, y - 1, z) && SkyCollision.isKnown(x, y - SkyCollision.REGION_SIZE, z);
		// (bridge #22) at a linked portal it is on its way through (EntityTransit): not held in front of it
		known = known || nearPortal(e.position(), 3.0);
		boolean held = HELD.containsKey(e.getId());
		if (!known && !held && !e.isNoGravity()) {
			HELD.put(e.getId(), true);
			e.setNoGravity(true);
			e.setDeltaMovement(e.getDeltaMovement().multiply(0, 0, 0));
		} else if (known && held) {
			HELD.remove(e.getId());
			e.setNoGravity(false);
		} else if (held) {
			e.setDeltaMovement(Vec3.ZERO);
		}
	}

	private static boolean nearPortal(Vec3 p, double r) {
		if (!dev.skycraft.world.PortalPair.anyLinked()) {
			return false;
		}
		for (int i = 0; i < dev.skycraft.world.PortalPair.SLOTS; i++) {
			Vec3[] pose = dev.skycraft.world.PortalPair.pose(i);
			if (pose != null && pose[0].distanceToSqr(p) < r * r) {
				return true;
			}
		}
		return false;
	}

	private static void applyHazards(ServerLevel level, Entity e) {
		AABB bb = e.getBoundingBox();
		Vec3 c = bb.getCenter();
		for (Hazard h : HAZARDS.values()) {
			if (!box(h).intersects(bb)) {
				continue;
			}
			if (h.kind == HAZARD_LASER && segmentDistance(c, h.a, h.b) > Math.max(bb.getXsize(), bb.getYsize()) * 0.5 + 0.3) {
				continue;
			}
			switch (h.kind) {
				case HAZARD_LETHAL -> {
					if (e instanceof LivingEntity le) {
						le.hurtServer(level, level.damageSources().genericKill(), Float.MAX_VALUE);
						kills++;
					} else {
						e.discard(); // goo takes dropped things too
						dissolved++;
					}
				}
				case HAZARD_FIZZLER -> {
					if (e instanceof LivingEntity le) {
						le.hurtServer(level, level.damageSources().magic(), Float.MAX_VALUE);
						kills++;
					} else {
						e.discard(); // an emancipation grid dissolves items and arrows
						dissolved++;
					}
				}
				case HAZARD_LASER -> {
					if (e instanceof LivingEntity le) {
						le.hurtServer(level, level.damageSources().inFire(), 2.0F);
						hurts++;
					}
				}
				default -> {
					if (e instanceof LivingEntity le) {
						le.hurtServer(level, level.damageSources().generic(), 2.0F);
						hurts++;
					}
				}
			}
			if (logs++ < 60) {
				SkyCraft.LOG.info("Portalcraft: Portal 2 hazard kind {} on {}", h.kind, e);
			}
			return;
		}
	}

	public static String stats() {
		return "host light cells " + LIGHT.size() + ", hazards " + HAZARDS.size() + ", held " + HELD.size() + ", hurts " + hurts + ", kills " + kills + ", dissolved " + dissolved;
	}

	private HostWorld() {
	}

	/** (bridge #53) the sizes of this class's static caches and queues, for the periodic growth line in the log. */
	public static String growth() {
		return "host light " + LIGHT.size() + ", hazards " + HAZARDS.size() + ", held " + HELD.size() + ", pending " + PENDING.size();
	}
}
