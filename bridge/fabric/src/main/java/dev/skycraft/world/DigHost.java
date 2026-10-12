package dev.skycraft.world;

import static dev.skycraft.link.Proto.*;
import static java.lang.foreign.ValueLayout.*;

import dev.skycraft.link.SkyLink;
import java.lang.foreign.MemorySegment;
import java.lang.invoke.VarHandle;

/**
 * What the host says about digging (Portalcraft, bridge #33): HostPlayer flag HP_DIG, digMaterial (the surface under the
 * crosshair) and digWorld (this map's dig world id). Cached for a couple of milliseconds; any thread.
 * With HP_DIG clear (every host before #33) {@link #enabled()} is false and digging does nothing at all.
 */
public final class DigHost {
	private DigHost() {
	}

	private static final VarHandle INT = JAVA_INT.varHandle();
	private static final long CACHE_NANOS = 2_000_000L;
	private static volatile long readAt;
	private static volatile int flags;
	private static volatile int world;
	private static volatile int material;

	private static void refresh() {
		long now = System.nanoTime();
		if (now - readAt < CACHE_NANOS) {
			return;
		}
		readAt = now;
		MemorySegment s = SkyLink.segment();
		if (s == null || SkyLink.hostVersion() < 17 || !SkyLink.active()) {
			flags = 0;
			world = 0;
			material = 0;
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
			int m = s.get(JAVA_INT, b + HPL_DIG_MATERIAL);
			int w = s.get(JAVA_INT, b + HPL_DIG_WORLD);
			VarHandle.loadLoadFence();
			if ((int) INT.getAcquire(s, b + HPL_SEQ) == seq1) {
				flags = f;
				material = m;
				world = w;
				return;
			}
		}
		// never written or kept tearing: keep the previous values (a host that never wrote HostPlayer has flags 0)
	}

	/** The host digs (HP_DIG) and has given this map a dig world. */
	public static boolean enabled() {
		refresh();
		return (flags & HP_DIG) != 0 && world != 0;
	}

	/** HP_DIG set (whether or not a world is given yet). */
	public static boolean flagSet() {
		refresh();
		return (flags & HP_DIG) != 0;
	}

	/** This map's dig world id, 0 if none (never dig). Only meaningful with HP_DIG. */
	public static int world() {
		refresh();
		return (flags & HP_DIG) != 0 ? world : 0;
	}

	/** HostPlayer.digMaterial: the host surface under the crosshair (low byte CH_*, bit 8 portalable). */
	public static int material() {
		refresh();
		return material;
	}
}
