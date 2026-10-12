package dev.skycraft.world;

import static dev.skycraft.link.Proto.*;
import static java.lang.foreign.ValueLayout.*;

import dev.skycraft.link.SkyLink;
import java.lang.foreign.MemorySegment;
import java.lang.invoke.VarHandle;

/**
 * What the host says about the Minecraft stage (Portalcraft, bridge #47): HostPlayer flag HP_STAGE, and the chunk the stage's
 * window is centred on (stageChunkX / stageChunkZ) and how far around it the stage is wanted (stageRadius, chunks).
 * Cached for a couple of milliseconds; any thread. With HP_STAGE clear (every host before #47, and every session that never
 * opened a portal onto Minecraft) the stage does nothing at all.
 */
public final class StageHost {
	private StageHost() {
	}

	private static final VarHandle INT = JAVA_INT.varHandle();
	private static final long CACHE_NANOS = 2_000_000L;
	private static volatile long readAt;
	private static volatile int flags;
	private static volatile int chunkX, chunkZ, radius;

	private static void refresh() {
		long now = System.nanoTime();
		if (now - readAt < CACHE_NANOS) {
			return;
		}
		readAt = now;
		MemorySegment s = SkyLink.segment();
		if (s == null || SkyLink.hostVersion() < 17 || !SkyLink.active()) {
			flags = 0;
			return;
		}
		long b = OFF_HOST_PLAYER;
		for (int attempt = 0; attempt < 8; attempt++) {
			int seq1 = (int) INT.getAcquire(s, b + HPL_SEQ);
			if (seq1 == 0) {
				break;
			}
			if ((seq1 & 1) != 0) {
				Thread.onSpinWait();
				continue;
			}
			int f = s.get(JAVA_INT, b + HPL_FLAGS);
			int cx = s.get(JAVA_INT, b + HPL_STAGE_CX);
			int cz = s.get(JAVA_INT, b + HPL_STAGE_CZ);
			int r = s.get(JAVA_INT, b + HPL_STAGE_R);
			VarHandle.loadLoadFence();
			if ((int) INT.getAcquire(s, b + HPL_SEQ) == seq1) {
				flags = f;
				chunkX = cx;
				chunkZ = cz;
				radius = r;
				return;
			}
		}
		// never written or kept tearing: keep the previous values
	}

	/** The host wants the stage (HP_STAGE) around its window. */
	public static boolean enabled() {
		refresh();
		return (flags & HP_STAGE) != 0;
	}

	public static int chunkX() {
		refresh();
		return chunkX;
	}

	public static int chunkZ() {
		refresh();
		return chunkZ;
	}

	/** Chunks around the window's centre chunk that get the stage (2..12, default 8). */
	public static int radius() {
		refresh();
		int r = radius;
		return r < 2 ? 8 : Math.min(r, 12);
	}
}
