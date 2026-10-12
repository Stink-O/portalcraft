package dev.skycraft.client.portalcraft;

import dev.skycraft.SkyCraft;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.UUID;
import net.minecraft.client.Minecraft;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.level.GameType;

/**
 * Portalcraft (bridge #19): a test run starts with a live, healthy Minecraft player. The world save is shared by every
 * test, and a run that ended on Minecraft's death screen left the next runs dead (the user's report, b19_scene4).
 *
 * <ul>
 * <li>Automatic: when Minecraft was started for a test (SKYCRAFT_TEST=1, set by tools/locked_run.sh), the first time the
 * player is in the world it is respawned if dead, then given full health and food, its effects and fire cleared, and
 * survival mode.</li>
 * <li>On request: the plugin's pcb_mc_reset_player writes cache/bridge/mc_reset_request (checked twice a second); the
 * same reset runs (tests end alive with it).</li>
 * </ul>
 * Normal play (play_clip.sh, no SKYCRAFT_TEST) never auto-respawns: the death screen is the user's.
 */
public final class TestReset {
	private static final boolean TEST = "1".equals(System.getenv("SKYCRAFT_TEST"));
	private static final Path REQUEST = Path.of(System.getProperty("portalcraft.root", "../../.."), "cache", "bridge", "mc_reset_request");
	private static boolean pending = TEST;
	private static long lastCheck, respawnAt;

	private TestReset() {
	}

	/** Start of every linked frame (render thread). */
	public static void frame(Minecraft minecraft) {
		long now = System.currentTimeMillis();
		if (now - lastCheck > 500) {
			lastCheck = now;
			try {
				if (Files.deleteIfExists(REQUEST)) {
					pending = true;
				}
			} catch (java.io.IOException ignored) {
			}
		}
		var player = minecraft.player;
		if (!pending || player == null || minecraft.level == null) {
			return;
		}
		if (player.isDeadOrDying()) {
			if (now - respawnAt > 1000) {
				respawnAt = now;
				player.respawn();
				minecraft.gui.setScreen(null);
				SkyCraft.LOG.info("Portalcraft: test reset: the player was dead; respawned");
			}
			return;
		}
		var server = minecraft.getSingleplayerServer();
		pending = false;
		if (server == null) {
			return;
		}
		UUID id = player.getUUID();
		server.execute(() -> {
			ServerPlayer sp = server.getPlayerList().getPlayer(id);
			if (sp == null) {
				return;
			}
			sp.setHealth(sp.getMaxHealth());
			sp.getFoodData().setFoodLevel(20);
			sp.getFoodData().setSaturation(5.0F);
			sp.removeAllEffects();
			// (bridge #31) tests: a strong Regeneration (hidden: no particles, no icon) so damage events are logged but a burst never ends the level
			sp.addEffect(new net.minecraft.world.effect.MobEffectInstance(net.minecraft.world.effect.MobEffects.REGENERATION, -1, 9, false, false, false));
			sp.clearFire();
			sp.setGameMode(GameType.SURVIVAL);
			SkyCraft.LOG.info("Portalcraft: test reset: the player is alive, full health and food, survival");
		});
	}
}
