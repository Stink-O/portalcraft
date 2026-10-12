package dev.skycraft.combat;

import dev.skycraft.SkyCraft;
import dev.skycraft.link.Proto;
import dev.skycraft.link.SkyLink;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.Iterator;
import java.util.List;
import java.util.Map;
import net.fabricmc.fabric.api.event.lifecycle.v1.ServerTickEvents;
import net.fabricmc.fabric.api.object.builder.v1.entity.FabricDefaultAttributeRegistry;
import net.minecraft.core.Registry;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.core.registries.Registries;
import net.minecraft.network.chat.Component;
import net.minecraft.resources.Identifier;
import net.minecraft.resources.ResourceKey;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.damagesource.DamageSource;
import net.minecraft.world.damagesource.DamageSources;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.entity.EntityType;
import net.minecraft.world.entity.LivingEntity;
import net.minecraft.world.entity.MobCategory;
import org.jspecify.annotations.Nullable;

/**
 * Combat between the Minecraft player and Skyrim actors, server side.
 *
 * <p>Every Skyrim actor near the player gets an invisible {@link SkyrimActorEntity} at its exact
 * position. Minecraft weapons hit those like any mob; the resulting damage is sent to Skyrim, which
 * applies it to the real actor (scaled by level) and makes it fight back. Skyrim's hits on the player
 * come back as Minecraft damage from the attacker's stand-in, so armor, shields, knockback, hurt
 * sounds and death all work the Minecraft way.
 */
public final class SkyCombat {
	public static final ResourceKey<EntityType<?>> SKYRIM_ACTOR_KEY =
		ResourceKey.create(Registries.ENTITY_TYPE, Identifier.fromNamespaceAndPath(SkyCraft.MOD_ID, "skyrim_actor"));
	public static final EntityType<SkyrimActorEntity> SKYRIM_ACTOR = Registry.register(
		BuiltInRegistries.ENTITY_TYPE,
		SKYRIM_ACTOR_KEY,
		EntityType.Builder.<SkyrimActorEntity>of(SkyrimActorEntity::new, MobCategory.MISC)
			.sized(0.6F, 1.8F)
			.noSave()
			.noSummon()
			.noLootTable()
			.clientTrackingRange(10)
			.updateInterval(1)
			.build(SKYRIM_ACTOR_KEY)
	);

	/** Skyrim damage is divided by this for Minecraft (a 15-damage bandit swing = 3 = 1.5 hearts). */
	public static final float SKYRIM_TO_MC_DAMAGE = 5.0F;

	private static final Map<Integer, SkyrimActorEntity> PROXIES = new HashMap<>();
	private static final List<SkyLink.Actor> ACTORS = new ArrayList<>();

	private SkyCombat() {
	}

	public static void init() {
		FabricDefaultAttributeRegistry.register(SKYRIM_ACTOR, LivingEntity.createLivingAttributes());
		ServerTickEvents.END_SERVER_TICK.register(SkyCombat::serverTick);
	}

	public static @Nullable SkyrimActorEntity proxy(int formId) {
		return PROXIES.get(formId);
	}

	private static void serverTick(MinecraftServer server) {
		List<ServerPlayer> players = server.getPlayerList().getPlayers();
		if (!SkyLink.active() || players.isEmpty()) {
			removeAll();
			return;
		}
		ServerLevel level = players.getFirst().level();
		for (ServerPlayer player : players) {
			pickUpNearby(player);
		}
		if (SkyLink.readActors(ACTORS)) {
			sync(level);
		}
		huntChell(level);
		dev.skycraft.portalcraft.HostWorld.serverTick(level, players); // Portalcraft (bridge #19): hazards, held mobs
		dev.skycraft.portalcraft.EntityTransit.tick(level); // Portalcraft (bridge #22): mobs and items through the portal pair
		runTestCommands(server);
		// Hits land during the tick (melee, sweeps, arrows, fire); send one combined hit per actor.
		for (SkyrimActorEntity proxy : PROXIES.values()) {
			float[] hit = proxy.takeHit();
			if (hit != null && (hit[0] > 0.0F || hit[3] > 0.0F)) {
				SkyLink.pushEvent(
					Proto.EV_HIT_ACTOR, proxy.formId(), hit[0], hit[1], hit[2], hit[3], Float.floatToRawIntBits(hit[4]), Float.floatToRawIntBits(hit[5])
				);
				SkyCraft.LOG.info("SkyCraft: hit {} for {} (knockback {})", proxy.getName().getString(), hit[0], hit[3]);
			}
		}
	}

	private static void sync(ServerLevel level) {
		Map<Integer, SkyLink.Actor> live = new HashMap<>();
		for (SkyLink.Actor a : ACTORS) {
			if (!a.dead()) {
				live.put(a.formId(), a);
			}
		}
		for (Iterator<Map.Entry<Integer, SkyrimActorEntity>> it = PROXIES.entrySet().iterator(); it.hasNext(); ) {
			Map.Entry<Integer, SkyrimActorEntity> e = it.next();
			SkyrimActorEntity proxy = e.getValue();
			if (!live.containsKey(e.getKey()) || proxy.isRemoved() || proxy.level() != level) {
				proxy.discard();
				it.remove();
			}
		}
		int before = PROXIES.size();
		for (SkyLink.Actor a : live.values()) {
			SkyrimActorEntity proxy = PROXIES.get(a.formId());
			if (proxy == null) {
				proxy = new SkyrimActorEntity(SKYRIM_ACTOR, level);
				proxy.setFormId(a.formId());
				proxy.setSize(a.width(), a.height());
				proxy.snapTo(a.x(), a.y(), a.z(), a.yaw(), 0.0F);
				if (!a.name().isEmpty()) {
					proxy.setCustomName(Component.literal(a.name()));
				}
				if (!level.addFreshEntity(proxy)) {
					continue;
				}
				PROXIES.put(a.formId(), proxy);
				continue;
			}
			double ox = proxy.getX(), oy = proxy.getY(), oz = proxy.getZ();
			proxy.setSize(a.width(), a.height());
			proxy.setPos(a.x(), a.y(), a.z());
			proxy.setYRot(a.yaw());
			proxy.setYHeadRot(a.yaw());
			stepOnTriggers(level, proxy);
			if (a.formId() == CHELL_FORM_ID) {
				chellSounds(level, proxy, ox, oy, oz);
			}
		}
		if (PROXIES.size() != before && (PROXIES.size() % 5 == 0 || PROXIES.size() < 5)) {
			SkyCraft.LOG.info("SkyCraft: {} Skyrim actors mirrored as hittable stand-ins", PROXIES.size());
		}
	}

	/**
	 * Portalcraft test hook: the bridge's test scripts queue Minecraft server commands (summon a mob,
	 * set the time) as lines in cache/bridge/mc_commands.txt; they run once and the file is removed.
	 */
	private static final java.nio.file.Path TEST_COMMANDS = java.nio.file.Path.of(System.getProperty("portalcraft.root", "../../.."), "cache", "bridge", "mc_commands.txt");

	private static void runTestCommands(MinecraftServer server) {
		if (server.getTickCount() % 10 != 0 || !java.nio.file.Files.exists(TEST_COMMANDS)) {
			return;
		}
		try {
			List<String> lines = java.nio.file.Files.readAllLines(TEST_COMMANDS);
			java.nio.file.Files.deleteIfExists(TEST_COMMANDS);
			for (String line : lines) {
				if (!line.isBlank()) {
					SkyCraft.LOG.info("Portalcraft: test command: {}", line.strip());
					server.getCommands().performPrefixedCommand(server.createCommandSourceStack().withSuppressedOutput(), line.strip());
				}
			}
		} catch (java.io.IOException e) {
			SkyCraft.LOG.warn("Portalcraft: test commands unreadable: {}", e.toString());
		}
	}

	/**
	 * Portalcraft: Chell's stand-in sounds like a player walking in Minecraft: the block's own step
	 * sound every ~1.7 blocks on the ground (Minecraft's player step rate), its fall sound and the
	 * landing thud after a drop, and a splash entering water. (Placed, not moved by physics, so
	 * Minecraft never plays these for it by itself.)
	 */
	private static double chellWalked, chellFallStartY = Double.NaN;
	private static boolean chellWasInWater;

	private static void chellSounds(ServerLevel level, SkyrimActorEntity proxy, double ox, double oy, double oz) {
		var below = net.minecraft.core.BlockPos.containing(proxy.getX(), proxy.getY() - 0.2, proxy.getZ());
		var state = level.getBlockState(below);
		boolean onGround = !state.isAir() && state.getFluidState().isEmpty() && proxy.getY() - (below.getY() + 1.0) < 0.25;
		boolean inWater = !level.getFluidState(net.minecraft.core.BlockPos.containing(proxy.getX(), proxy.getY() + 0.3, proxy.getZ())).isEmpty();
		double dx = proxy.getX() - ox, dz = proxy.getZ() - oz;
		double h = Math.sqrt(dx * dx + dz * dz);
		if (h > 3.0) {  // a teleport (portal), not a step
			h = 0;
		}
		if (inWater && !chellWasInWater && oy - proxy.getY() > 0.1) {
			level.playSound(null, proxy.getX(), proxy.getY(), proxy.getZ(), net.minecraft.sounds.SoundEvents.PLAYER_SPLASH, net.minecraft.sounds.SoundSource.PLAYERS, 0.6F, 1.0F);
		}
		chellWasInWater = inWater;
		if (!onGround) {
			if (Double.isNaN(chellFallStartY) || proxy.getY() > chellFallStartY) {
				chellFallStartY = proxy.getY();
			}
			chellWalked = 0;
			return;
		}
		if (!Double.isNaN(chellFallStartY)) {
			double fell = chellFallStartY - proxy.getY();
			chellFallStartY = Double.NaN;
			if (fell > 1.5) {
				var type = state.getSoundType();
				level.playSound(null, proxy.getX(), proxy.getY(), proxy.getZ(), type.getFallSound(), net.minecraft.sounds.SoundSource.PLAYERS, type.getVolume() * 0.5F, type.getPitch() * 0.75F);
				level.playSound(null, proxy.getX(), proxy.getY(), proxy.getZ(),
					fell > 4.0 ? net.minecraft.sounds.SoundEvents.PLAYER_BIG_FALL : net.minecraft.sounds.SoundEvents.PLAYER_SMALL_FALL, net.minecraft.sounds.SoundSource.PLAYERS, 0.8F, 1.0F);
				return;
			}
		}
		chellWalked += h;
		if (chellWalked >= 1.0 / 0.6) {
			chellWalked = 0;
			var type = state.getSoundType();
			level.playSound(null, proxy.getX(), proxy.getY(), proxy.getZ(), type.getStepSound(), net.minecraft.sounds.SoundSource.PLAYERS, type.getVolume() * 0.15F, type.getPitch());
		}
	}

	/** Portalcraft: the host's player (Chell in Portal 2) is mirrored with this id. */
	public static final int CHELL_FORM_ID = 0x0C4E11;
	private static int huntLog;
	private static final boolean DEBUG_HUNT = Boolean.getBoolean("portalcraft.debugHunt") || java.nio.file.Files.exists(java.nio.file.Path.of(System.getProperty("portalcraft.root", "../../.."), "cache", "bridge", "debug_hunt"));

	/**
	 * Portalcraft: hostile mobs hunt Chell's stand-in the way they hunt a player (the hidden Minecraft
	 * player is in creative, so they ignore it). A monster within 32 blocks that has no target and can
	 * see her takes her as its target; its own goals do the rest (zombies path and swing, skeletons
	 * shoot, creepers swell), and every hit reaches Chell as Portal 2 damage through the proxy.
	 */
	private static void huntChell(ServerLevel level) {
		SkyrimActorEntity chell = PROXIES.get(CHELL_FORM_ID);
		if (chell == null || chell.isRemoved() || level.getGameTime() % 10 != 0) {
			return;
		}
		var box = chell.getBoundingBox().inflate(32.0);
		for (net.minecraft.world.entity.monster.Monster m : level.getEntitiesOfClass(net.minecraft.world.entity.monster.Monster.class, box)) {
			if (!m.isAlive()) {
				continue;
			}
			if (m.getTarget() == chell) {
				if (DEBUG_HUNT && level.getGameTime() % 40 == 0) {
					SkyCraft.LOG.info("Portalcraft: hunt debug {}: dist {} sees {} using item {} main hand {}", m.getName().getString(), String.format("%.1f", m.distanceTo(chell)),
						m.getSensing().hasLineOfSight(chell), m.isUsingItem(), m.getMainHandItem());
					// (bridge #22) where its navigation is going: no path, or a path that ends short of her
					var path = m.getNavigation().getPath();
					var fresh = m.getNavigation().createPath(chell, 0);
					SkyCraft.LOG.info("Portalcraft: hunt debug {} at {} -> Chell at {}: path {} (end {}, reaches {}), fresh path {} (end {}, reaches {})", m.getName().getString(),
						m.blockPosition(), chell.blockPosition(), path != null, path != null ? path.getEndNode() : null, path != null && path.canReach(), fresh != null,
						fresh != null ? fresh.getEndNode() : null, fresh != null && fresh.canReach());
				}
				continue;
			}
			var t = m.getTarget();
			if (t != null && t.isAlive()) {
				continue;
			}
			if (!m.getSensing().hasLineOfSight(chell)) {
				continue;
			}
			m.setTarget(chell);
			if (huntLog++ < 20) {
				SkyCraft.LOG.info("Portalcraft: {} hunts Chell ({} blocks away)", m.getName().getString(), String.format("%.1f", m.distanceTo(chell)));
			}
		}
	}

	/**
	 * Skyrim's NPCs press pressure plates and trip tripwires. Their stand-ins are placed, not moved
	 * (no physics), so Minecraft never checks what they step into; do it for those blocks here.
	 */
	private static void stepOnTriggers(ServerLevel level, SkyrimActorEntity proxy) {
		var box = proxy.getBoundingBox().deflate(1.0E-5);
		var from = net.minecraft.core.BlockPos.containing(box.minX, box.minY, box.minZ);
		var to = net.minecraft.core.BlockPos.containing(box.maxX, box.maxY, box.maxZ);
		for (var pos : net.minecraft.core.BlockPos.betweenClosed(from, to)) {
			var state = level.getBlockState(pos);
			if (state.getBlock() instanceof net.minecraft.world.level.block.BasePressurePlateBlock
				|| state.getBlock() instanceof net.minecraft.world.level.block.TripWireBlock) {
				state.entityInside(level, pos, proxy, net.minecraft.world.entity.InsideBlockEffectApplier.NOOP, true);
			}
		}
	}

	/**
	 * Items and stuck arrows on Skyrim ground rest on its collision voxels, which on steep or rough
	 * terrain can sit a little off from where the player (on Skyrim's exact triangles) stands.
	 * Touch them over a slightly bigger area than vanilla's so walking over them picks them up.
	 * playerTouch applies all of Minecraft's own rules (pickup delay, owner, inventory space).
	 */
	private static void pickUpNearby(ServerPlayer player) {
		if (!player.isAlive() || player.isSpectator()) {
			return;
		}
		for (Entity entity : player.level().getEntities(player, player.getBoundingBox().inflate(1.25, 1.0, 1.25))) {
			if (!entity.isRemoved() && (entity instanceof net.minecraft.world.entity.item.ItemEntity
				|| entity instanceof net.minecraft.world.entity.projectile.arrow.AbstractArrow)) {
				entity.playerTouch(player);
			}
		}
	}

	private static void removeAll() {
		if (PROXIES.isEmpty()) {
			return;
		}
		PROXIES.values().forEach(Entity::discard);
		PROXIES.clear();
	}

	/**
	 * Skyrim hit the player. Runs on the server thread. {@code kind} is a Proto.HURT_* value and
	 * {@code skyrimDamage} is what Skyrim would have taken off the player's health.
	 */
	/** Portalcraft (bridge #57): set only around a host-damage hurtServer; LivingEntityKnockbackMixin cancels knockback while it is true. */
	public static volatile boolean NO_KNOCKBACK;

	public static void hurtPlayer(ServerPlayer player, int kind, float skyrimDamage, int attackerFormId, int flags) {
		if (!player.isAlive() || skyrimDamage <= 0.0F) {
			return;
		}
		ServerLevel level = player.level();
		SkyrimActorEntity attacker = PROXIES.get(attackerFormId);
		if (attacker != null && attacker.distanceToSqr(player) > 24.0 * 24.0) {
			attacker = null; // a guest's own NPC with the same form id as one of the host's
		}
		DamageSources sources = level.damageSources();
		DamageSource source = switch (kind) {
			case Proto.HURT_MELEE -> attacker != null ? sources.mobAttack(attacker) : sources.generic();
			case Proto.HURT_PROJECTILE -> attacker != null ? sources.mobProjectile(attacker, attacker) : sources.generic();
			case Proto.HURT_MAGIC -> attacker != null ? sources.indirectMagic(attacker, attacker) : sources.magic();
			default -> hostSource(sources, (flags >>> 8) & 0xFF);
		};
		float damage = skyrimDamage / SKYRIM_TO_MC_DAMAGE;
		float healthBefore = player.getHealth();
		boolean blocking = player.isBlocking();
		if (kind != Proto.HURT_MELEE && kind != Proto.HURT_PROJECTILE && kind != Proto.HURT_MAGIC) {
			// Portalcraft (bridge #55): Portal 2's damage has no invulnerability frames. Minecraft's 10-tick grace after a hit would swallow a
			// laser's steady damage (a hit no bigger than the last one is ignored inside it), so every host hit lands in full.
			((dev.skycraft.mixin.EntityInvulnerableTimeAccessor) player).skycraft$setInvulnerableTime(0);
		}
		// Portalcraft (bridge #57): host damage (laser, turret, goo, crush, fall...) moves nobody in Minecraft. hurtServer's default knockback
		// (0.4 blocks/tick, up when on the ground, a random direction when the source has no position) landed on every hit, which with the
		// invulnerability cleared above was ~20 hits a second: a fling up several blocks. The red flash, sound and tilt are untouched.
		boolean hostHit = kind != Proto.HURT_MELEE && kind != Proto.HURT_PROJECTILE && kind != Proto.HURT_MAGIC;
		NO_KNOCKBACK = hostHit;
		boolean hurt;
		try {
			hurt = player.hurtServer(level, source, damage);
		} finally {
			NO_KNOCKBACK = false;
		}
		trainDefence(player, damage, blocking && player.getHealth() >= healthBefore - 1.0E-3F);
		SkyCraft.LOG.info("SkyCraft: Skyrim hit the player for {} ({} Minecraft): health {} -> {}{}", skyrimDamage, damage, healthBefore, player.getHealth(),
			hurt ? "" : " (blocked/immune)");
		if (hurt && attacker != null && (flags & Proto.HURT_POWER_ATTACK) != 0 && !player.isBlocking()) {
			// Power attacks shove harder, like a sprint hit does in Minecraft.
			player.knockback(0.5, attacker.getX() - player.getX(), attacker.getZ() - player.getZ(), source, damage);
		}
	}

	/**
	 * Portalcraft (bridge #32): the damage source for Portal 2's damage to the player. The host sends the class of what hurt the body
	 * (flags bits 8..15 of kInHurt; 0 = generic, what older hosts send): 1 crush, 2 burn (laser), 3 goo, 4 drown, 5 blast, 6 shock,
	 * 7 fall, 8 a kill volume's lethal damage.
	 */
	private static DamageSource hostSource(DamageSources sources, int hostClass) {
		return switch (hostClass) {
			case 1 -> sources.cramming();
			case 2 -> sources.onFire();
			case 3 -> sources.lava();
			case 4 -> sources.drown();
			case 5 -> sources.explosion(null, null);
			case 6 -> sources.lightningBolt();
			case 7 -> sources.fall();
			case 8 -> sources.fellOutOfWorld();
			default -> sources.generic();
		};
	}

	/**
	 * Skyrim skills for taking a hit: Block when the shield caught it, otherwise Light or Heavy
	 * Armor by what the player mostly wears (leather, chainmail, gold, copper and turtle count as
	 * light; iron, diamond and netherite as heavy). Only the host's own Skyrim is told.
	 */
	private static void trainDefence(ServerPlayer player, float damage, boolean blocked) {
		if (!dev.skycraft.net.SkyNet.isHost(player) || damage <= 0.0F) {
			return;
		}
		if (blocked) {
			SkyLink.pushEvent(Proto.EV_SKILL_USE, Proto.SKILL_BLOCK, damage, 0.0F, 0.0F, 0.0F, 0);
			return;
		}
		int light = 0, heavy = 0;
		for (var slot : new net.minecraft.world.entity.EquipmentSlot[] { net.minecraft.world.entity.EquipmentSlot.HEAD, net.minecraft.world.entity.EquipmentSlot.CHEST,
			net.minecraft.world.entity.EquipmentSlot.LEGS, net.minecraft.world.entity.EquipmentSlot.FEET }) {
			var stack = player.getItemBySlot(slot);
			if (stack.isEmpty()) {
				continue;
			}
			String path = net.minecraft.core.registries.BuiltInRegistries.ITEM.getKey(stack.getItem()).getPath();
			if (path.startsWith("iron_") || path.startsWith("diamond_") || path.startsWith("netherite_")) {
				heavy++;
			} else {
				light++;
			}
		}
		if (light + heavy > 0) {
			SkyLink.pushEvent(Proto.EV_SKILL_USE, heavy > light ? Proto.SKILL_HEAVY_ARMOR : Proto.SKILL_LIGHT_ARMOR, damage * (light + heavy) / 4.0F, 0.0F, 0.0F,
				0.0F, 0);
		}
	}

	/** Form id of the Skyrim actor behind a damage source, or 0. */
	public static int attackerFormId(DamageSource source) {
		return source.getEntity() instanceof SkyrimActorEntity proxy ? proxy.formId() : 0;
	}

	/** (bridge #53) the sizes of this class's static caches and queues, for the periodic growth line in the log. */
	public static String growth() {
		return "proxies " + PROXIES.size() + ", actors " + ACTORS.size();
	}
}
