package dev.skycraft.client.portalcraft;

import dev.skycraft.link.Proto;
import dev.skycraft.link.SkyLink;
import net.minecraft.client.Camera;
import net.minecraft.client.Minecraft;

/**
 * Portalcraft (bridge #16): the Minecraft player's body and its F5 camera with Portal 2 as the host.
 *
 * <ul>
 * <li>The host asks for the body's mesh (kRenAvatar) in first person too (HostPlayer kHpWantAvatar): Portal 2 draws it
 * in its portal views, so the player sees itself through portals, as Chell does.</li>
 * <li>While the body is in the region (kHpAvatarInMc), Minecraft draws its own player in the host-view passes that do
 * not look out of its eyes: every pass in third person, and in first person the portal views (a clip plane, not the
 * host's main view) and the views Minecraft renders through the live pair itself (PortalPasses). Anywhere else (the
 * body in the chamber band, high above the region; or Chell is the player) Minecraft never draws it in a host pass.</li>
 * <li>{@link #zoom}: the third-person camera distance Minecraft's own camera chose this frame (its zoom collision
 * against blocks and the host's exported collision), recorded before a host view replaces the camera's position.</li>
 * </ul>
 */
public final class AvatarHost {
	private static final int[] HOST_PLAYER = new int[5];
	private static long readAt;
	private static int flags;
	private static volatile float zoom;

	private AvatarHost() {
	}

	private static int flags() {
		long now = System.nanoTime();
		if (now - readAt > 2_000_000L) { // at most every 2 ms (it is read per pass)
			readAt = now;
			flags = SkyLink.readHostPlayer(HOST_PLAYER) ? HOST_PLAYER[0] : 0;
		}
		return flags;
	}

	/** (bridge #34) The next flag read is fresh (a new host teleport was just seen: the cached flags are up to 2 ms old). */
	public static void refreshFlags() {
		readAt = 0;
	}

	/** (bridge #28) The host lets Minecraft transit its player through the portal pair (an older host transits the body itself). */
	public static boolean hostWantsTransit() {
		return (flags() & Proto.HP_MC_TRANSIT) != 0;
	}

	/** (bridge #31) The host is handing its body to Minecraft and the movers around it are not in Minecraft yet: a held player stays on the body. */
	public static boolean hostHandOver() {
		return (flags() & Proto.HP_HAND_OVER) != 0;
	}

	/** The host wants the body's mesh every frame (first person included). */
	public static boolean wantAvatar() {
		int f = flags();
		return (f & Proto.HP_MC_MODE) != 0 && (f & Proto.HP_WANT_AVATAR) != 0;
	}

	/**
	 * Whether the current pass draws the local player (LevelExtractor's "camera entity" test). {@code vanilla} is
	 * Minecraft's own answer (third person). Only host-view passes are changed.
	 */
	public static boolean drawLocalPlayer(Camera camera, boolean vanilla) {
		if (!HostViewClient.active()) {
			return vanilla;
		}
		int f = flags();
		if ((f & Proto.HP_MC_MODE) == 0 || (f & Proto.HP_AVATAR_IN_MC) == 0) {
			return false; // Chell is the player (the hidden player follows her), or the body is in the chamber band
		}
		if (vanilla || HostViewClient.pass() >= 2) {
			return true; // third person, or a view Minecraft renders through the live pair
		}
		return (HostViewClient.view().flags & Proto.VIEW_FULL_SCREEN) == 0; // a portal view, not the eyes
	}

	public static void setZoom(float z) {
		zoom = z;
	}

	/** The third-person camera's distance from the eye (blocks) after Minecraft's zoom collision; 0 in first person. */
	public static float zoom(Minecraft minecraft) {
		return minecraft.options.getCameraType().isFirstPerson() ? 0.0F : zoom;
	}
}
