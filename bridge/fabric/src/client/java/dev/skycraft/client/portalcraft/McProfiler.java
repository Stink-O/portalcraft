package dev.skycraft.client.portalcraft;

import dev.skycraft.SkyCraft;
import java.nio.file.Files;
import java.nio.file.Path;
import net.minecraft.client.Minecraft;

/**
 * Portalcraft (measurements): Minecraft's own metrics recorder (the F3+L debug profiling: per-section
 * render-thread timings plus the integrated server's) started and stopped by the host. The plugin's
 * pcb_mc_profile writes cache/bridge/mc_profile_request; each request toggles recording. The report zip
 * lands in fabric/run/debug/profiling/, and its path is logged.
 */
public final class McProfiler {
	private static final Path REQUEST = Path.of(System.getProperty("portalcraft.root", "../../.."), "cache", "bridge", "mc_profile_request");
	private static long lastCheck;

	private McProfiler() {
	}

	/** Start of every linked frame (render thread); checks the request file twice a second. */
	public static void frame(Minecraft minecraft) {
		long now = System.currentTimeMillis();
		if (now - lastCheck < 500) {
			return;
		}
		lastCheck = now;
		try {
			if (!Files.deleteIfExists(REQUEST)) {
				return;
			}
		} catch (java.io.IOException e) {
			return;
		}
		boolean started = minecraft.debugClientMetricsStart(c -> SkyCraft.LOG.info("Portalcraft: profiler: {}", c.getString()));
		SkyCraft.LOG.info("Portalcraft: profiler {}", started ? "started" : "stopping");
	}
}
