package dev.skycraft.link;

import static dev.skycraft.link.Proto.*;
import static java.lang.foreign.ValueLayout.*;

import dev.skycraft.SkyCraft;
import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.lang.invoke.VarHandle;
import java.nio.charset.StandardCharsets;

/**
 * The Minecraft end of the shared-memory link. Skyrim owns the mapping; we open it when it
 * appears and treat it as gone when Skyrim's heartbeat stops.
 */
public final class SkyLink {
	// Portalcraft (Linux port): the host plugin creates POSIX shared memory (shm_open) that
	// shows up as a file under /dev/shm; we map that file. Clocks are CLOCK_MONOTONIC on both
	// sides (System.nanoTime() on Linux), replacing GetTickCount64 / QueryPerformanceCounter.
	// Portal 2's loading screens can stall its heartbeat for several seconds.
	private static final long HEARTBEAT_TIMEOUT_MS = 8000;

	private static final VarHandle INT = JAVA_INT.varHandle();
	private static final VarHandle LONG = JAVA_LONG.varHandle();

	private static java.nio.channels.FileChannel runningLockChannel;

	/** /dev/shm path of the mapping for a POSIX shm name like "/Portalcraft_v1". */
	private static java.nio.file.Path shmPath(String name) {
		return java.nio.file.Path.of("/dev/shm", name.startsWith("/") ? name.substring(1) : name);
	}

	/**
	 * Holds an exclusive lock on /dev/shm/<name>_minecraft for as long as this Minecraft runs, so the
	 * host plugin knows one is already running (even before the two have linked up).
	 */
	public static synchronized void announceRunning() {
		if (WINDOWS) {
			WinMapping.announceRunning(MAPPING_NAME + "_minecraft");  // the named-mutex form of the same promise
			return;
		}
		if (runningLockChannel != null) {
			return;
		}
		try {
			var ch = java.nio.channels.FileChannel.open(shmPath(MAPPING_NAME + "_minecraft"),
				java.nio.file.StandardOpenOption.CREATE, java.nio.file.StandardOpenOption.WRITE);
			if (ch.tryLock() != null) {
				runningLockChannel = ch;
			} else {
				ch.close();
				SkyCraft.LOG.warn("SkyCraft: another Minecraft already holds the running lock");
			}
		} catch (Throwable t) {
			SkyCraft.LOG.warn("SkyCraft: couldn't create the running-Minecraft lock", t);
		}
	}

	private static volatile MemorySegment shm;
	private static long lastOpenAttempt;
	private static Object mappedKey;
	private static int skyrimPid;
	private static volatile int generation;

	private SkyLink() {
	}

	/** True when a live Skyrim is on the other end. Cheap; safe from any thread. */
	public static boolean active() {
		MemorySegment s = shm;
		if (s == null) {
			return false;
		}
		long beat = (long) LONG.getAcquire(s, OFF_HEADER + H_SKYRIM_HEARTBEAT);
		return tickCount() - beat < HEARTBEAT_TIMEOUT_MS;
	}

	/** Bumps whenever a (new) Skyrim instance is on the other end: everything Skyrim caches must be resent. */
	public static int generation() {
		return generation;
	}

	/** Process id of the Skyrim we're linked to (0 before the first link). */
	public static int skyrimPid() {
		return skyrimPid;
	}

	/** The mapping if it has been opened (whether or not Skyrim is still alive). */
	public static MemorySegment segment() {
		return shm;
	}

	/** Try to open the mapping at most once a second. Call regularly from the render thread. */
	public static void poll() {
		// Portalcraft: the host recreates the mapping every time Portal 2 starts. If our mapping has
		// been dead for a while and a (new) one exists, drop ours so it is reopened below.
		if (!WINDOWS && shm != null && !active() && System.currentTimeMillis() - lastOpenAttempt > 10_000) {
			try {
				Object key = java.nio.file.Files.readAttributes(shmPath(MAPPING_NAME), java.nio.file.attribute.BasicFileAttributes.class).fileKey();
				if (key != null && !key.equals(mappedKey)) {
					SkyCraft.LOG.info("SkyCraft: host mapping was recreated; relinking");
					shm = null;
				}
			} catch (java.io.IOException e) {
				// no mapping right now: keep waiting on the old one
			}
			lastOpenAttempt = System.currentTimeMillis();
		}
		if (shm != null) {
			LONG.setRelease(shm, OFF_HEADER + H_MC_HEARTBEAT, tickCount());
			int pid = shm.get(JAVA_INT, OFF_HEADER + H_SKYRIM_PID);
			if (pid != skyrimPid) {
				// Skyrim restarted and reset the shared state; start our side over too.
				skyrimPid = pid;
				overlayBack = 1;
				hudBack = 1;
				generation++;
				SkyCraft.LOG.info("SkyCraft: Skyrim instance changed (pid {})", pid);
			}
			return;
		}
		long now = System.currentTimeMillis();
		if (now - lastOpenAttempt < 1000) {
			return;
		}
		lastOpenAttempt = now;
		if (WINDOWS) {
			MemorySegment view = WinMapping.open(MAPPING_NAME);
			if (view != null && view.byteSize() >= MAPPING_BYTES_V16) {
				try {
					adopt(view.byteSize() > MAPPING_BYTES ? view.asSlice(0, MAPPING_BYTES) : view, Math.min(view.byteSize(), MAPPING_BYTES), null, MAPPING_NAME);
				} catch (Throwable t) {
					SkyCraft.LOG.error("SkyCraft: failed to open shared memory", t);
				}
			}
			return;
		}
		java.nio.file.Path path = shmPath(MAPPING_NAME);
		if (!java.nio.file.Files.exists(path)) {
			return;
		}
		try (var ch = java.nio.channels.FileChannel.open(path, java.nio.file.StandardOpenOption.READ, java.nio.file.StandardOpenOption.WRITE)) {
			if (ch.size() < MAPPING_BYTES_V16) {
				return; // the host is still sizing it
			}
			// The mapping outlives the channel; Arena.global() keeps it mapped for the process lifetime.
			// v17 hosts add the HUD triple buffer at the end; a v16 host's file stops before it.
			long mapBytes = Math.min(ch.size(), MAPPING_BYTES);
			MemorySegment seg = ch.map(java.nio.channels.FileChannel.MapMode.READ_WRITE, 0, mapBytes, Arena.global());
			adopt(seg, mapBytes, java.nio.file.Files.readAttributes(path, java.nio.file.attribute.BasicFileAttributes.class).fileKey(), path.toString());
		} catch (Throwable t) {
			SkyCraft.LOG.error("SkyCraft: failed to open shared memory", t);
		}
	}

	/** Validates a freshly mapped host mapping and, if it is a live host of a known protocol version, makes it the link. */
	private static void adopt(MemorySegment seg, long mapBytes, Object key, String what) {
		int magic = seg.get(JAVA_INT, OFF_HEADER + H_MAGIC);
		int version = seg.get(JAVA_INT, OFF_HEADER + H_VERSION);
		// v12 only adds fields in reserved slot-header bytes and a new slot flag, so a v11 host
		// (the stable plugin build) links too and ignores them
		// v13 adds the HostViewExt block (read only from v13 hosts) and the second slot image
		if (magic != MAGIC || version < 11 || version > VERSION) {
			SkyCraft.LOG.error("SkyCraft: protocol mismatch (magic {} version {}); expected version {}", Integer.toHexString(magic), version, VERSION);
			return;
		}
		int hostPid = seg.get(JAVA_INT, OFF_HEADER + H_SKYRIM_PID);
		if (!ProcessHandle.of(hostPid).map(ProcessHandle::isAlive).orElse(false)) {
			return; // a mapping left behind by a host that crashed: wait for a live one
		}
		seg.set(JAVA_INT, OFF_HEADER + H_MC_PID, (int) ProcessHandle.current().pid());
		LONG.setRelease(seg, OFF_HEADER + H_MC_HEARTBEAT, tickCount());
		skyrimPid = seg.get(JAVA_INT, OFF_HEADER + H_SKYRIM_PID);
		hostVersion = version;
		hudMapped = mapBytes >= MAPPING_BYTES;
		hudBack = 1;
		mappedKey = key;
		generation++;
		shm = seg;
		SkyCraft.LOG.info("SkyCraft: linked to the host game (pid {}) via {}", skyrimPid, what);
	}

	/** CLOCK_MONOTONIC in nanoseconds: the same clock the host plugin reads, so tick timestamps line up. */
	public static long qpc() {
		return System.nanoTime();
	}

	/** Ticks per second of {@link #qpc()}. */
	public static long qpcFrequency() {
		return 1_000_000_000L;
	}

	/** CLOCK_MONOTONIC in milliseconds (the heartbeat clock). */
	public static long tickCount() {
		return System.nanoTime() / 1_000_000L;
	}

	// ---- SkyState (read) -------------------------------------------------------------------

	/** Plain snapshot of SkyState. */
	public static final class SkyState {
		public int seq;
		public int flags;
		public int worldId;
		public int transitAck; // bridge #28: the host's look includes the turns of transits up to this seq
		public double x, y, z;
		public float yaw, pitch;
		public int teleportSeq;
		public int viewportW, viewportH;
		public float gameHour;

		public boolean inGame() {
			return (this.flags & SKY_IN_GAME) != 0;
		}

		public boolean menuOpen() {
			return (this.flags & SKY_MENU_OPEN) != 0;
		}

		public boolean loading() {
			return (this.flags & SKY_LOADING) != 0;
		}
	}

	/** Seqlock read of SkyState into {@code out}. Returns false if the link is down. */
	/** Skyrim's water surface around the player (see WaterGrid in the protocol). */
	public static final class WaterGrid {
		public int originX, originZ, worldId;
		public final int size = WATER_GRID_SIZE;
		public final float[] surface = new float[WATER_GRID_SIZE * WATER_GRID_SIZE];
	}

	/** A consistent copy of the water grid, or null (no link, or Skyrim mid-write). */
	public static WaterGrid readWaterGrid() {
		MemorySegment s = shm;
		if (s == null) {
			return null;
		}
		long b = OFF_WATER_GRID;
		WaterGrid out = new WaterGrid();
		for (int attempt = 0; attempt < 100; attempt++) {
			int seq1 = (int) INT.getAcquire(s, b + WG_SEQ);
			if ((seq1 & 1) != 0 || seq1 == 0) {
				Thread.onSpinWait();
				if (seq1 == 0) {
					return null; // Skyrim hasn't written one yet
				}
				continue;
			}
			out.originX = s.get(JAVA_INT, b + WG_ORIGIN_X);
			out.originZ = s.get(JAVA_INT, b + WG_ORIGIN_Z);
			out.worldId = s.get(JAVA_INT, b + WG_WORLD_ID);
			for (int i = 0; i < out.surface.length; i++) {
				out.surface[i] = s.get(JAVA_FLOAT, b + WG_SURFACE + i * 4L);
			}
			VarHandle.loadLoadFence();
			if ((int) INT.getAcquire(s, b + WG_SEQ) == seq1) {
				return out;
			}
		}
		return null;
	}

	public static boolean readSkyState(SkyState out) {
		MemorySegment s = shm;
		if (s == null) {
			return false;
		}
		long b = OFF_SKY_STATE;
		for (int attempt = 0; attempt < 1000; attempt++) {
			int seq1 = (int) INT.getAcquire(s, b + SS_SEQ);
			if ((seq1 & 1) != 0) {
				if (attempt > 100) {
					Thread.yield();
				} else {
					Thread.onSpinWait();
				}
				continue;
			}
			out.flags = s.get(JAVA_INT, b + SS_FLAGS);
			out.worldId = s.get(JAVA_INT, b + SS_WORLD_ID);
			out.transitAck = s.get(JAVA_INT, b + SS_TRANSIT_ACK);
			out.x = s.get(JAVA_DOUBLE, b + SS_POS_X);
			out.y = s.get(JAVA_DOUBLE, b + SS_POS_Y);
			out.z = s.get(JAVA_DOUBLE, b + SS_POS_Z);
			out.yaw = s.get(JAVA_FLOAT, b + SS_YAW);
			out.pitch = s.get(JAVA_FLOAT, b + SS_PITCH);
			out.teleportSeq = s.get(JAVA_INT, b + SS_TELEPORT_SEQ);
			out.viewportW = s.get(JAVA_INT, b + SS_VIEWPORT_W);
			out.viewportH = s.get(JAVA_INT, b + SS_VIEWPORT_H);
			out.gameHour = s.get(JAVA_FLOAT, b + SS_GAME_HOUR);
			VarHandle.loadLoadFence();
			int seq2 = (int) INT.getAcquire(s, b + SS_SEQ);
			if (seq1 == seq2) {
				out.seq = seq1;
				return true;
			}
		}
		return false;
	}

	/** Raw SkyState sequence number; changes once per Skyrim frame. */
	/** Portalcraft: the host camera Minecraft renders its own frame from (see HostView in the protocol). */
	public static final class HostView {
		public int seq;
		public int flags;
		public double x, y, z;
		public float yaw, pitch, roll, fovY;
		public final float[] plane = new float[4];
		public int requestId;

		public boolean active() {
			return (this.flags & VIEW_ACTIVE) != 0;
		}
	}

	/** Seqlock read of the HostView; false if the link is down or the read kept tearing. */
	public static boolean readHostView(HostView out) {
		MemorySegment s = shm;
		if (s == null) {
			return false;
		}
		long b = OFF_HOST_VIEW;
		for (int attempt = 0; attempt < 1000; attempt++) {
			int seq1 = (int) INT.getAcquire(s, b + HV_SEQ);
			if ((seq1 & 1) != 0) {
				Thread.onSpinWait();
				continue;
			}
			out.flags = s.get(JAVA_INT, b + HV_FLAGS);
			out.x = s.get(JAVA_DOUBLE, b + HV_X);
			out.y = s.get(JAVA_DOUBLE, b + HV_Y);
			out.z = s.get(JAVA_DOUBLE, b + HV_Z);
			out.yaw = s.get(JAVA_FLOAT, b + HV_YAW);
			out.pitch = s.get(JAVA_FLOAT, b + HV_PITCH);
			out.roll = s.get(JAVA_FLOAT, b + HV_ROLL);
			out.fovY = s.get(JAVA_FLOAT, b + HV_FOV_Y);
			for (int i = 0; i < 4; i++) {
				out.plane[i] = s.get(JAVA_FLOAT, b + HV_PLANE + 4L * i);
			}
			out.requestId = s.get(JAVA_INT, b + HV_REQUEST_ID);
			VarHandle.loadLoadFence();
			int seq2 = (int) INT.getAcquire(s, b + HV_SEQ);
			if (seq1 == seq2) {
				out.seq = seq1;
				return true;
			}
		}
		return false;
	}

	private static volatile int hostVersion;

	/** The linked host's protocol version (11, 12 or 13). */
	/** Portalcraft v14: the host's audio listener (seqlock). False if the host is older or the read kept tearing. */
	/** Portalcraft (bridge #23): HostAudio flags (AUDIO_*), or 0. Read without the seqlock: a torn read is one frame's flags. */
	public static int hostAudioFlags() {
		MemorySegment s = shm;
		return s == null || hostVersion < 14 ? 0 : s.get(JAVA_INT, OFF_HOST_AUDIO + HA_FLAGS);
	}

	/** Portalcraft (bridge #23): the host's level before a test mute. */
	public static float hostAudioLevel() {
		MemorySegment s = shm;
		return s == null || hostVersion < 14 ? 0.0F : s.get(JAVA_FLOAT, OFF_HOST_AUDIO + HA_LEVEL);
	}

	public static boolean readHostAudio(double[] pos, float[] yawPitchGain) {
		MemorySegment s = shm;
		if (s == null || hostVersion < 14) {
			return false;
		}
		long b = OFF_HOST_AUDIO;
		for (int attempt = 0; attempt < 8; attempt++) {
			int seq1 = (int) INT.getAcquire(s, b + HA_SEQ);
			if ((seq1 & 1) != 0) {
				Thread.onSpinWait();
				continue;
			}
			int flags = s.get(JAVA_INT, b + HA_FLAGS);
			pos[0] = s.get(JAVA_DOUBLE, b + HA_X);
			pos[1] = s.get(JAVA_DOUBLE, b + HA_Y);
			pos[2] = s.get(JAVA_DOUBLE, b + HA_Z);
			yawPitchGain[0] = s.get(JAVA_FLOAT, b + HA_YAW);
			yawPitchGain[1] = s.get(JAVA_FLOAT, b + HA_PITCH);
			yawPitchGain[2] = (flags & AUDIO_ACTIVE) != 0 ? s.get(JAVA_FLOAT, b + HA_GAIN) : 0.0F;
			VarHandle.loadLoadFence();
			if ((int) INT.getAcquire(s, b + HA_SEQ) == seq1) {
				return true;
			}
		}
		return false;
	}

	/**
	 * Portalcraft v16: the host's light probes (seqlock). Fills pos (3 doubles per probe) and returns
	 * the probe count, with the request id in idOut[0]; -1 if the host is older, never wrote it, or
	 * the read kept tearing.
	 */
	public static int readLightProbes(double[] pos, int[] idOut) {
		MemorySegment s = shm;
		if (s == null || hostVersion < 16) {
			return -1;
		}
		long b = OFF_LIGHT_PROBES;
		for (int attempt = 0; attempt < 8; attempt++) {
			int seq1 = (int) INT.getAcquire(s, b + LP_SEQ);
			if ((seq1 & 1) != 0 || seq1 == 0) {
				if (seq1 == 0) {
					return -1;
				}
				Thread.onSpinWait();
				continue;
			}
			int n = Math.max(0, Math.min(MAX_LIGHT_PROBES, s.get(JAVA_INT, b + LP_COUNT)));
			idOut[0] = s.get(JAVA_INT, b + LP_REQUEST_ID);
			for (int i = 0; i < n * 3; i++) {
				pos[i] = s.get(JAVA_DOUBLE, b + LP_POS + 8L * i);
			}
			VarHandle.loadLoadFence();
			if ((int) INT.getAcquire(s, b + LP_SEQ) == seq1) {
				return n;
			}
		}
		return -1;
	}

	/** Portalcraft v16: Minecraft's light at the host's probes (seqlock write). samples: 6 floats per probe (r g b sky block skyVisible). */
	public static void writeMcLight(int requestId, int count, float[] sunDir, float daylight, float[] skyColor, float dayTime, float[] skyLightColor, float[] samples) {
		MemorySegment s = shm;
		if (s == null || hostVersion < 16) {
			return;
		}
		long b = OFF_MC_LIGHT;
		int seq = s.get(JAVA_INT, b + ML_SEQ);
		INT.setRelease(s, b + ML_SEQ, seq + 1);
		VarHandle.storeStoreFence();
		s.set(JAVA_INT, b + ML_REQUEST_ID, requestId);
		s.set(JAVA_INT, b + ML_COUNT, count);
		s.set(JAVA_INT, b + ML_FLAGS, 0);
		for (int i = 0; i < 3; i++) {
			s.set(JAVA_FLOAT, b + ML_SUN_DIR + 4L * i, sunDir[i]);
			s.set(JAVA_FLOAT, b + ML_SKY_COLOR + 4L * i, skyColor[i]);
			s.set(JAVA_FLOAT, b + ML_SKY_LIGHT_COLOR + 4L * i, skyLightColor[i]);
		}
		s.set(JAVA_FLOAT, b + ML_DAYLIGHT, daylight);
		s.set(JAVA_FLOAT, b + ML_DAY_TIME, dayTime);
		for (int i = 0; i < count; i++) {
			long e = b + ML_SAMPLES + LIGHT_SAMPLE_BYTES * i;
			s.set(JAVA_FLOAT, e + LS_R, samples[i * 6]);
			s.set(JAVA_FLOAT, e + LS_G, samples[i * 6 + 1]);
			s.set(JAVA_FLOAT, e + LS_B, samples[i * 6 + 2]);
			s.set(JAVA_FLOAT, e + LS_SKY, samples[i * 6 + 3]);
			s.set(JAVA_FLOAT, e + LS_BLOCK, samples[i * 6 + 4]);
			s.set(JAVA_FLOAT, e + LS_SKY_VISIBLE, samples[i * 6 + 5]);
		}
		INT.setRelease(s, b + ML_SEQ, seq + 2);
	}

	public static int hostVersion() {
		return hostVersion;
	}

	/** Portalcraft v13: crop windows and the second camera (HostViewExt). */
	public static final class HostViewExt {
		public int seq, flags;
		public final float[] crop = new float[4];
		public int primaryRequestId, primaryStream;
		public final HostView second = new HostView();
		public final float[] scrop = new float[4];
		public int secStream, secRenderDistance;

		public boolean valid() {
			return (this.flags & EXT_VALID) != 0;
		}
	}

	/** Seqlock read of the HostViewExt (v13 hosts only); false if absent, torn or never written. */
	public static boolean readHostViewExt(HostViewExt out) {
		MemorySegment s = shm;
		if (s == null || hostVersion < 13) {
			return false;
		}
		long b = OFF_HOST_VIEW_EXT;
		for (int attempt = 0; attempt < 1000; attempt++) {
			int seq1 = (int) INT.getAcquire(s, b + HX_SEQ);
			if ((seq1 & 1) != 0) {
				Thread.onSpinWait();
				continue;
			}
			out.flags = s.get(JAVA_INT, b + HX_FLAGS);
			for (int i = 0; i < 4; i++) {
				out.crop[i] = s.get(JAVA_FLOAT, b + HX_CROP + 4L * i);
				out.scrop[i] = s.get(JAVA_FLOAT, b + HX_SCROP + 4L * i);
				out.second.plane[i] = s.get(JAVA_FLOAT, b + HX_SPLANE + 4L * i);
			}
			out.primaryRequestId = s.get(JAVA_INT, b + HX_PRIMARY_REQUEST_ID);
			out.primaryStream = s.get(JAVA_INT, b + HX_PRIMARY_STREAM);
			HostView v = out.second;
			v.x = s.get(JAVA_DOUBLE, b + HX_SX);
			v.y = s.get(JAVA_DOUBLE, b + HX_SY);
			v.z = s.get(JAVA_DOUBLE, b + HX_SZ);
			v.yaw = s.get(JAVA_FLOAT, b + HX_SYAW);
			v.pitch = s.get(JAVA_FLOAT, b + HX_SPITCH);
			v.roll = s.get(JAVA_FLOAT, b + HX_SROLL);
			v.fovY = s.get(JAVA_FLOAT, b + HX_SFOV_Y);
			v.requestId = s.get(JAVA_INT, b + HX_SEC_REQUEST_ID);
			v.flags = s.get(JAVA_INT, b + HX_SEC_FLAGS);
			out.secStream = s.get(JAVA_INT, b + HX_SEC_STREAM);
			out.secRenderDistance = s.get(JAVA_INT, b + HX_SEC_RENDER_DISTANCE);
			VarHandle.loadLoadFence();
			int seq2 = (int) INT.getAcquire(s, b + HX_SEQ);
			if (seq1 == seq2) {
				out.seq = seq1;
				return true;
			}
		}
		return false;
	}

	/** Portalcraft v15: one portal of the live pair (HostPortal). */
	public static final class HostPortal {
		public double cx, cy, cz;
		public float nx, ny, nz, ux, uy, uz;
		public float halfW, halfH, open;
		public int colour;
	}

	/** Portalcraft v15: the live portal pair Minecraft draws itself (HostPortals). */
	public static final class HostPortals {
		public int seq, flags, primaryRequestId, maxDepth, renderDistance;
		public float resScale;
		public final HostPortal[] portal = {new HostPortal(), new HostPortal()};

		public boolean inMc() {
			return (this.flags & PORTALS_IN_MC) != 0;
		}
	}

	/** Seqlock read of the HostPortals block (v15 hosts only); false if absent or torn. */
	public static boolean readHostPortals(HostPortals out) {
		MemorySegment s = shm;
		if (s == null || hostVersion < 15) {
			return false;
		}
		long b = OFF_HOST_PORTALS;
		for (int attempt = 0; attempt < 1000; attempt++) {
			int seq1 = (int) INT.getAcquire(s, b + HP_SEQ);
			if ((seq1 & 1) != 0) {
				Thread.onSpinWait();
				continue;
			}
			out.flags = s.get(JAVA_INT, b + HP_FLAGS);
			out.primaryRequestId = s.get(JAVA_INT, b + HP_PRIMARY_REQUEST_ID);
			out.maxDepth = s.get(JAVA_INT, b + HP_MAX_DEPTH);
			out.renderDistance = s.get(JAVA_INT, b + HP_RENDER_DISTANCE);
			out.resScale = s.get(JAVA_FLOAT, b + HP_RES_SCALE);
			for (int i = 0; i < 2; i++) {
				long p = b + HP_PORTAL + HOST_PORTAL_BYTES * i;
				HostPortal o = out.portal[i];
				o.cx = s.get(JAVA_DOUBLE, p + PO_CX);
				o.cy = s.get(JAVA_DOUBLE, p + PO_CY);
				o.cz = s.get(JAVA_DOUBLE, p + PO_CZ);
				o.nx = s.get(JAVA_FLOAT, p + PO_NX);
				o.ny = s.get(JAVA_FLOAT, p + PO_NY);
				o.nz = s.get(JAVA_FLOAT, p + PO_NZ);
				o.ux = s.get(JAVA_FLOAT, p + PO_UX);
				o.uy = s.get(JAVA_FLOAT, p + PO_UY);
				o.uz = s.get(JAVA_FLOAT, p + PO_UZ);
				o.halfW = s.get(JAVA_FLOAT, p + PO_HALF_W);
				o.halfH = s.get(JAVA_FLOAT, p + PO_HALF_H);
				o.open = s.get(JAVA_FLOAT, p + PO_OPEN);
				o.colour = s.get(JAVA_INT, p + PO_COLOUR);
			}
			VarHandle.loadLoadFence();
			if ((int) INT.getAcquire(s, b + HP_SEQ) == seq1) {
				out.seq = seq1;
				return true;
			}
		}
		return false;
	}

	/** Portalcraft v13: a second image in the slot about to be published (call before publishWorldView). */
	public static void setWorldViewImage2(long offset, int width, int height, int requestId, boolean depthAlpha) {
		MemorySegment s = shm;
		if (s == null) {
			return;
		}
		long hdr = OFF_OVERLAY_SLOT_HDR + overlayBack * SLOT_HDR_SIZE;
		s.set(JAVA_INT, hdr + SH_IMG2_OFFSET, (int) offset);
		s.set(JAVA_SHORT, hdr + SH_IMG2_W, (short) width);
		s.set(JAVA_SHORT, hdr + SH_IMG2_H, (short) height);
		s.set(JAVA_INT, hdr + SH_IMG2_REQUEST_ID, requestId);
		s.set(JAVA_INT, hdr + SH_IMG2_FLAGS, offset != 0 ? (SLOT_BOTTOM_UP | SLOT_WORLD_VIEW | (depthAlpha ? SLOT_DEPTH_ALPHA : 0)) : 0);
	}

	/** Portalcraft: publish a world-view frame (colour, then float depth at depthOffset; 0 = none). */
	public static void publishWorldView(int width, int height, long frameId, int requestId, long depthOffset, float zNear, float zFar) {
		publishWorldView(width, height, frameId, requestId, depthOffset, zNear, zFar, 0, 0);
	}

	public static void publishWorldView(int width, int height, long frameId, int requestId, long depthOffset, float zNear, float zFar, int depthW, int depthH) {
		publishWorldView(width, height, frameId, requestId, depthOffset, zNear, zFar, depthW, depthH, false);
	}

	public static void publishWorldView(int width, int height, long frameId, int requestId, long depthOffset, float zNear, float zFar, int depthW, int depthH, boolean depthAlpha) {
		MemorySegment s = shm;
		if (s == null) {
			return;
		}
		long hdr = OFF_OVERLAY_SLOT_HDR + overlayBack * SLOT_HDR_SIZE;
		s.set(JAVA_INT, hdr + SH_VIEW_REQUEST_ID, requestId);
		s.set(JAVA_INT, hdr + SH_DEPTH_OFFSET, (int) depthOffset);
		s.set(JAVA_FLOAT, hdr + SH_Z_NEAR, zNear);
		s.set(JAVA_FLOAT, hdr + SH_Z_FAR, zFar);
		s.set(JAVA_SHORT, hdr + SH_DEPTH_W, (short) depthW);
		s.set(JAVA_SHORT, hdr + SH_DEPTH_H, (short) depthH);
		s.set(JAVA_INT, hdr + SH_WIDTH, width);
		s.set(JAVA_INT, hdr + SH_HEIGHT, height);
		s.set(JAVA_INT, hdr + SH_FLAGS, SLOT_BOTTOM_UP | SLOT_WORLD_VIEW | (depthAlpha ? SLOT_DEPTH_ALPHA : 0));
		s.set(JAVA_LONG, hdr + SH_FRAME_ID, frameId);
		int old = (int) INT.getAndSet(s, OFF_OVERLAY_CTL + OC_STATE, overlayBack | OVERLAY_DIRTY);
		overlayBack = old & 3;
		LONG.getAndAdd(s, OFF_OVERLAY_CTL + OC_FRAMES_PUBLISHED, 1L);
	}

	public static int skyStateSeq() {
		MemorySegment s = shm;
		return s == null ? 0 : (int) INT.getAcquire(s, OFF_SKY_STATE + SS_SEQ);
	}

	// ---- McState (write) -------------------------------------------------------------------

	public static final class McState {
		public int flags;
		public double x, y, z;
		public float yaw, pitch;
		public float eyeHeight;
		public float sensitivity;
		public int teleportAck;
		public int guiScale;
		public long frameCounter;
		public float fov;
		public float bobPhase;
		public float bobAmount;
		public double eyeX, eyeY, eyeZ;
		public long tickQpc;
		public double prevX, prevY, prevZ;
		public double curX, curY, curZ;
		public float eyeHeightO, eyeHeightT;
		public float walkDistO, walkDist;
		public float bobO, bob;
		public float tickMs = 50.0F;
		public int cameraMode;
		public float cameraDistance;
		// Portalcraft v18 (bridge #26): the mover the player rode this tick (see McState in the protocol header)
		public int rideMover, rideFlags;
		public double ridePrevX, ridePrevY, ridePrevZ, rideCurX, rideCurY, rideCurZ;
		// Portalcraft (bridge #28): the latest transit of the player through the portal pair (McTransit in the protocol)
		public int transitSeq, transitPortals;
		public final float[] transitQ = { 0, 0, 0, 1 };
		public final double[] transitOff = new double[3];
		public float transitFrac;
	}

	public static void writeMcState(McState st) {
		MemorySegment s = shm;
		if (s == null) {
			return;
		}
		long b = OFF_MC_STATE;
		int seq = s.get(JAVA_INT, b + MS_SEQ);
		INT.setRelease(s, b + MS_SEQ, seq + 1);
		VarHandle.storeStoreFence();
		s.set(JAVA_INT, b + MS_FLAGS, st.flags);
		s.set(JAVA_DOUBLE, b + MS_X, st.x);
		s.set(JAVA_DOUBLE, b + MS_Y, st.y);
		s.set(JAVA_DOUBLE, b + MS_Z, st.z);
		s.set(JAVA_FLOAT, b + MS_YAW, st.yaw);
		s.set(JAVA_FLOAT, b + MS_PITCH, st.pitch);
		s.set(JAVA_FLOAT, b + MS_EYE_HEIGHT, st.eyeHeight);
		s.set(JAVA_FLOAT, b + MS_SENSITIVITY, st.sensitivity);
		s.set(JAVA_INT, b + MS_TELEPORT_ACK, st.teleportAck);
		s.set(JAVA_INT, b + MS_GUI_SCALE, st.guiScale);
		s.set(JAVA_LONG, b + MS_FRAME_COUNTER, st.frameCounter);
		s.set(JAVA_FLOAT, b + MS_FOV, st.fov);
		s.set(JAVA_FLOAT, b + MS_BOB_PHASE, st.bobPhase);
		s.set(JAVA_FLOAT, b + MS_BOB_AMOUNT, st.bobAmount);
		s.set(JAVA_DOUBLE, b + MS_EYE_X, st.eyeX);
		s.set(JAVA_DOUBLE, b + MS_EYE_Y, st.eyeY);
		s.set(JAVA_DOUBLE, b + MS_EYE_Z, st.eyeZ);
		s.set(JAVA_LONG, b + MS_TICK_QPC, st.tickQpc);
		s.set(JAVA_DOUBLE, b + MS_PREV_X, st.prevX);
		s.set(JAVA_DOUBLE, b + MS_PREV_X + 8, st.prevY);
		s.set(JAVA_DOUBLE, b + MS_PREV_X + 16, st.prevZ);
		s.set(JAVA_DOUBLE, b + MS_CUR_X, st.curX);
		s.set(JAVA_DOUBLE, b + MS_CUR_X + 8, st.curY);
		s.set(JAVA_DOUBLE, b + MS_CUR_X + 16, st.curZ);
		s.set(JAVA_FLOAT, b + MS_EYE_HEIGHT_O, st.eyeHeightO);
		s.set(JAVA_FLOAT, b + MS_EYE_HEIGHT_T, st.eyeHeightT);
		s.set(JAVA_FLOAT, b + MS_WALK_O, st.walkDistO);
		s.set(JAVA_FLOAT, b + MS_WALK, st.walkDist);
		s.set(JAVA_FLOAT, b + MS_BOB_O, st.bobO);
		s.set(JAVA_FLOAT, b + MS_BOB, st.bob);
		s.set(JAVA_FLOAT, b + MS_TICK_MS, st.tickMs);
		s.set(JAVA_INT, b + MS_RIDE_MOVER, st.rideMover);
		s.set(JAVA_INT, b + MS_RIDE_FLAGS, st.rideFlags);
		s.set(JAVA_DOUBLE, b + MS_RIDE_PREV_X, st.ridePrevX);
		s.set(JAVA_DOUBLE, b + MS_RIDE_PREV_X + 8, st.ridePrevY);
		s.set(JAVA_DOUBLE, b + MS_RIDE_PREV_X + 16, st.ridePrevZ);
		s.set(JAVA_DOUBLE, b + MS_RIDE_CUR_X, st.rideCurX);
		s.set(JAVA_DOUBLE, b + MS_RIDE_CUR_X + 8, st.rideCurY);
		s.set(JAVA_DOUBLE, b + MS_RIDE_CUR_X + 16, st.rideCurZ);
		s.set(JAVA_INT, b + MS_CAMERA_MODE, st.cameraMode);
		s.set(JAVA_FLOAT, b + MS_CAMERA_DISTANCE, st.cameraDistance);
		s.set(JAVA_INT, b + MS_TRANSIT_SEQ, st.transitSeq);
		s.set(JAVA_INT, b + MS_TRANSIT_PORTALS, st.transitPortals);
		for (int i = 0; i < 4; i++) {
			s.set(JAVA_FLOAT, OFF_MC_TRANSIT + MT_Q + i * 4L, st.transitQ[i]);
		}
		for (int i = 0; i < 3; i++) {
			s.set(JAVA_DOUBLE, OFF_MC_TRANSIT + MT_OFF + i * 8L, st.transitOff[i]);
		}
		s.set(JAVA_FLOAT, OFF_MC_TRANSIT + MT_FRAC, st.transitFrac);
		s.set(JAVA_INT, OFF_MC_TRANSIT + MT_SEQ, st.transitSeq);
		INT.setRelease(s, b + MS_SEQ, seq + 2);
	}

	// ---- input ring (consume) --------------------------------------------------------------

	public interface InputSink {
		void accept(int type, int code, int a, int b, int c);
	}

	/** Drains every pending input event. Render thread only. */
	public static void drainInput(InputSink sink) {
		MemorySegment s = shm;
		if (s == null) {
			return;
		}
		long base = OFF_INPUT_RING;
		long head = (long) LONG.getAcquire(s, base + IR_HEAD);
		long tail = s.get(JAVA_LONG, base + IR_TAIL);
		if (head - tail > INPUT_RING_ENTRIES) {
			tail = head - INPUT_RING_ENTRIES; // producer lapped us; drop the oldest
		}
		while (tail < head) {
			long e = base + IR_DATA + (tail & (INPUT_RING_ENTRIES - 1)) * 16L;
			int type = Short.toUnsignedInt(s.get(JAVA_SHORT, e));
			int code = Short.toUnsignedInt(s.get(JAVA_SHORT, e + 2));
			int a = s.get(JAVA_INT, e + 4);
			int b = s.get(JAVA_INT, e + 8);
			int c = s.get(JAVA_INT, e + 12);
			tail++;
			sink.accept(type, code, a, b, c);
		}
		LONG.setRelease(s, base + IR_TAIL, tail);
	}

	// ---- actor table (read) ----------------------------------------------------------------

	/** One nearby Skyrim actor (see ActorRecord in the protocol header). */
	public record Actor(int formId, int flags, float x, float y, float z, float yaw, float width, float height, float healthFrac, int level, String name) {
		public boolean dead() {
			return (this.flags & ACTOR_DEAD) != 0;
		}

		public boolean hostile() {
			return (this.flags & ACTOR_HOSTILE) != 0;
		}
	}

	/** Seqlock read of the actor table. Returns false (leaving {@code out} empty) on a torn read. */
	public static boolean readActors(java.util.List<Actor> out) {
		out.clear();
		MemorySegment s = shm;
		if (s == null) {
			return false;
		}
		long b = OFF_ACTOR_TABLE;
		for (int attempt = 0; attempt < 16; attempt++) {
			int seq1 = (int) INT.getAcquire(s, b + AT_SEQ);
			if ((seq1 & 1) != 0) {
				Thread.onSpinWait();
				continue;
			}
			int count = Math.min(s.get(JAVA_INT, b + AT_COUNT), MAX_ACTORS);
			for (int i = 0; i < count; i++) {
				long r = b + AT_RECORDS + i * ACTOR_RECORD_BYTES;
				out.add(new Actor(
					s.get(JAVA_INT, r), s.get(JAVA_INT, r + 4),
					s.get(JAVA_FLOAT, r + 8), s.get(JAVA_FLOAT, r + 12), s.get(JAVA_FLOAT, r + 16),
					s.get(JAVA_FLOAT, r + 20), s.get(JAVA_FLOAT, r + 24), s.get(JAVA_FLOAT, r + 28),
					s.get(JAVA_FLOAT, r + 32), Short.toUnsignedInt(s.get(JAVA_SHORT, r + 36)), readName(s, r + 40, 24)
				));
			}
			VarHandle.loadLoadFence();
			if ((int) INT.getAcquire(s, b + AT_SEQ) == seq1) {
				return true;
			}
			out.clear();
		}
		return false;
	}

	private static String readName(MemorySegment s, long off, int max) {
		byte[] bytes = new byte[max];
		int n = 0;
		while (n < max) {
			byte b = s.get(JAVA_BYTE, off + n);
			if (b == 0) {
				break;
			}
			bytes[n++] = b;
		}
		return new String(bytes, 0, n, StandardCharsets.UTF_8);
	}

	// ---- event ring (produce) --------------------------------------------------------------

	/** Queues an event for Skyrim. Safe from any thread. Drops the event if Skyrim is a full ring behind. */
	public static void pushEvent(int type, int formId, float a, float b, float c, float d, int flags) {
		pushEvent(type, formId, a, b, c, d, flags, 0);
	}

	public static synchronized void pushEvent(int type, int formId, float a, float b, float c, float d, int flags, int weapon) {
		MemorySegment s = shm;
		if (s == null) {
			return;
		}
		long base = OFF_EVENT_RING;
		long head = s.get(JAVA_LONG, base + ER_HEAD);
		long tail = (long) LONG.getAcquire(s, base + ER_TAIL);
		if (head - tail >= EVENT_RING_ENTRIES) {
			return;
		}
		long e = base + ER_DATA + (head & (EVENT_RING_ENTRIES - 1)) * EVENT_BYTES;
		s.set(JAVA_INT, e, type);
		s.set(JAVA_INT, e + 4, formId);
		s.set(JAVA_FLOAT, e + 8, a);
		s.set(JAVA_FLOAT, e + 12, b);
		s.set(JAVA_FLOAT, e + 16, c);
		s.set(JAVA_FLOAT, e + 20, d);
		s.set(JAVA_INT, e + 24, flags);
		s.set(JAVA_INT, e + 28, weapon);
		LONG.setRelease(s, base + ER_HEAD, head + 1);
	}

	// ---- world entities (write) ------------------------------------------------------------

	/**
	 * One Minecraft thing for Skyrim to draw (see WorldEntity in the protocol header). {@code uv}
	 * holds up to three atlas rects {u0, v0, u1, v1}: sprite/side, top, bottom.
	 */
	public record WorldEntity(int kind, int id, float x, float y, float z, float yaw, float pitch, float scale, float[] ext, float[] uv, int tint) {
	}

	/** Seqlock write of the world-entity table and block selection. Render thread only. */
	public static void writeWorldEntities(java.util.List<WorldEntity> entities, float[] selection) {
		MemorySegment s = shm;
		if (s == null) {
			return;
		}
		long b = OFF_WORLD_ENTITIES;
		int seq = s.get(JAVA_INT, b + WE_SEQ);
		INT.setRelease(s, b + WE_SEQ, seq + 1);
		VarHandle.storeStoreFence();
		int count = Math.min(entities.size(), MAX_WORLD_ENTITIES);
		s.set(JAVA_INT, b + WE_COUNT, count);
		s.set(JAVA_INT, b + WE_HAS_SELECTION, selection != null ? 1 : 0);
		if (selection != null) {
			for (int i = 0; i < 6; i++) {
				s.set(JAVA_FLOAT, b + WE_SEL_MIN + i * 4L, selection[i]);
			}
		}
		for (int i = 0; i < count; i++) {
			WorldEntity w = entities.get(i);
			long r = b + WE_RECORDS + i * WORLD_ENTITY_BYTES;
			s.set(JAVA_INT, r, w.kind());
			s.set(JAVA_INT, r + 4, w.id());
			s.set(JAVA_FLOAT, r + 8, w.x());
			s.set(JAVA_FLOAT, r + 12, w.y());
			s.set(JAVA_FLOAT, r + 16, w.z());
			s.set(JAVA_FLOAT, r + 20, w.yaw());
			s.set(JAVA_FLOAT, r + 24, w.pitch());
			s.set(JAVA_FLOAT, r + 28, w.scale());
			for (int k = 0; k < 3; k++) {
				s.set(JAVA_FLOAT, r + 32 + k * 4L, w.ext() != null ? w.ext()[k] : 0.0F);
			}
			for (int k = 0; k < 12; k++) {
				s.set(JAVA_FLOAT, r + 44 + k * 4L, w.uv() != null && k < w.uv().length ? w.uv()[k] : 0.0F);
			}
			s.set(JAVA_INT, r + 92, w.tint());
		}
		INT.setRelease(s, b + WE_SEQ, seq + 2);
	}

	// ---- render ring (produce) -------------------------------------------------------------

	/**
	 * Writes one render message ({@code header} bytes then {@code body} bytes) into the render ring,
	 * waiting briefly for space. Single producer. Returns false if it never fit.
	 */
	public static boolean writeRender(int type, java.nio.ByteBuffer header, java.nio.ByteBuffer body) {
		return writeRender(type, header, body, 500);
	}

	/** Like writeRender, but gives up at once if the ring is full (per-frame data that the next frame replaces). */
	public static boolean tryWriteRender(int type, java.nio.ByteBuffer header, java.nio.ByteBuffer body) {
		return writeRender(type, header, body, 1);
	}

	private static synchronized boolean writeRender(int type, java.nio.ByteBuffer header, java.nio.ByteBuffer body, int attempts) {
		MemorySegment s = shm;
		if (s == null) {
			return false;
		}
		int payload = header.remaining() + (body != null ? body.remaining() : 0);
		long msgBytes = (8 + payload + 7) & ~7L;
		if (msgBytes > RR_DATA_BYTES / 2) {
			SkyCraft.LOG.warn("SkyCraft: render message too large ({} bytes)", msgBytes);
			return false;
		}
		long base = OFF_RENDER_RING;
		for (int attempt = 0; attempt < attempts; attempt++) {
			long head = s.get(JAVA_LONG, base + RR_HEAD);
			long tail = (long) LONG.getAcquire(s, base + RR_TAIL);
			long pos = head % RR_DATA_BYTES;
			long pad = pos + msgBytes > RR_DATA_BYTES ? RR_DATA_BYTES - pos : 0;
			if (RR_DATA_BYTES - (head - tail) < msgBytes + pad) {
				if (attempt + 1 >= attempts) {
					break;
				}
				try {
					Thread.sleep(2);
				} catch (InterruptedException e) {
					return false;
				}
				continue;
			}
			if (pad > 0) {
				s.set(JAVA_INT, base + RR_DATA + pos, REN_PAD);
				s.set(JAVA_INT, base + RR_DATA + pos + 4, 0);
				head += pad;
				pos = 0;
			}
			long at = base + RR_DATA + pos;
			s.set(JAVA_INT, at, type);
			s.set(JAVA_INT, at + 4, payload);
			MemorySegment.copy(MemorySegment.ofBuffer(header), 0, s, at + 8, header.remaining());
			if (body != null && body.remaining() > 0) {
				MemorySegment.copy(MemorySegment.ofBuffer(body), 0, s, at + 8 + header.remaining(), body.remaining());
			}
			LONG.setRelease(s, base + RR_HEAD, head + msgBytes);
			return true;
		}
		return false;
	}

	// ---- overlay (publish) -----------------------------------------------------------------

	private static int overlayBack = 1; // writer's private slot; Skyrim's front starts at 2, middle at 0

	/** Returns the byte offset where the next overlay frame should be written. */
	public static long overlayBackSlotOffset() {
		return OFF_OVERLAY_PIXELS + overlayBack * OVERLAY_SLOT_BYTES;
	}

	/** Publishes the frame just written into the back slot. */
	public static void publishOverlay(int width, int height, boolean bottomUp, long frameId) {
		MemorySegment s = shm;
		if (s == null) {
			return;
		}
		long hdr = OFF_OVERLAY_SLOT_HDR + overlayBack * SLOT_HDR_SIZE;
		s.set(JAVA_INT, hdr + SH_WIDTH, width);
		s.set(JAVA_INT, hdr + SH_HEIGHT, height);
		s.set(JAVA_INT, hdr + SH_FLAGS, bottomUp ? 1 : 0);
		s.set(JAVA_LONG, hdr + SH_FRAME_ID, frameId);
		int old = (int) INT.getAndSet(s, OFF_OVERLAY_CTL + OC_STATE, overlayBack | OVERLAY_DIRTY);
		overlayBack = old & 3;
		LONG.getAndAdd(s, OFF_OVERLAY_CTL + OC_FRAMES_PUBLISHED, 1L);
	}

	// ---- Portalcraft v17: HostPlayer (read) and the HUD triple buffer (publish) -------------

	private static volatile boolean hudMapped;
	private static int hudBack = 1; // writer's private slot, as overlayBack

	/**
	 * v18 (bridge #26): the host's McMovers block (seqlock). Fills ids[i] and pose[7 * i ..] = x, y, z (MC, absolute), qx, qy,
	 * qz, qw for each listed mover; returns the count, or -1 if the host is older, never wrote it, or the read kept tearing.
	 * epochOut[0] = the collision epoch the ids belong to.
	 */
	public static int readMovers(int[] ids, double[] pose, int[] epochOut) {
		MemorySegment s = shm;
		if (s == null || hostVersion < 18) {
			return -1;
		}
		long b = OFF_MC_MOVERS;
		for (int attempt = 0; attempt < 8; attempt++) {
			int seq1 = (int) INT.getAcquire(s, b + MV_SEQ);
			if ((seq1 & 1) != 0 || seq1 == 0) {
				if (seq1 == 0) {
					return -1;
				}
				Thread.onSpinWait();
				continue;
			}
			int n = Math.min(Math.min(s.get(JAVA_INT, b + MV_COUNT), MAX_MOVERS), ids.length);
			epochOut[0] = s.get(JAVA_INT, b + MV_EPOCH);
			double bx = s.get(JAVA_DOUBLE, b + MV_BASE_X), by = s.get(JAVA_DOUBLE, b + MV_BASE_X + 8), bz = s.get(JAVA_DOUBLE, b + MV_BASE_X + 16);
			for (int i = 0; i < n; i++) {
				long r = b + MV_POSES + i * MOVER_POSE_BYTES;
				ids[i] = s.get(JAVA_INT, r + MP_ID);
				pose[7 * i] = bx + s.get(JAVA_FLOAT, r + MP_X);
				pose[7 * i + 1] = by + s.get(JAVA_FLOAT, r + MP_X + 4);
				pose[7 * i + 2] = bz + s.get(JAVA_FLOAT, r + MP_X + 8);
				pose[7 * i + 3] = s.get(JAVA_FLOAT, r + MP_QX);
				pose[7 * i + 4] = s.get(JAVA_FLOAT, r + MP_QX + 4);
				pose[7 * i + 5] = s.get(JAVA_FLOAT, r + MP_QX + 8);
				pose[7 * i + 6] = s.get(JAVA_FLOAT, r + MP_QW);
			}
			VarHandle.loadLoadFence();
			if ((int) INT.getAcquire(s, b + MV_SEQ) == seq1) {
				return n;
			}
		}
		return -1;
	}

	/** True when the host is v17+ and the HUD buffer is mapped. */
	public static boolean hudAvailable() {
		return shm != null && hostVersion >= 17 && hudMapped;
	}

	/**
	 * v17: the host's HostPlayer block (seqlock): out = {flags, screenW, screenH, cursorX, cursorY}. False if the
	 * host is older, never wrote it, or the read kept tearing.
	 */
	public static boolean readHostPlayer(int[] out) {
		MemorySegment s = shm;
		if (s == null || hostVersion < 17) {
			return false;
		}
		long b = OFF_HOST_PLAYER;
		for (int attempt = 0; attempt < 8; attempt++) {
			int seq1 = (int) INT.getAcquire(s, b + HPL_SEQ);
			if ((seq1 & 1) != 0 || seq1 == 0) {
				if (seq1 == 0) {
					return false;
				}
				Thread.onSpinWait();
				continue;
			}
			out[0] = s.get(JAVA_INT, b + HPL_FLAGS);
			out[1] = s.get(JAVA_INT, b + HPL_SCREEN_W);
			out[2] = s.get(JAVA_INT, b + HPL_SCREEN_H);
			out[3] = s.get(JAVA_INT, b + HPL_CURSOR_X);
			out[4] = s.get(JAVA_INT, b + HPL_CURSOR_Y);
			VarHandle.loadLoadFence();
			if ((int) INT.getAcquire(s, b + HPL_SEQ) == seq1) {
				return true;
			}
		}
		return false;
	}

	/**
	 * Bridge #27: the host's light at the Minecraft player's hand (out[0..2]: Portal 2's linear light x tonemap scale; out[3]:
	 * the tonemap scale). Returns false when the host has not set one (Minecraft lights the hand itself).
	 */
	public static boolean readHostHandLight(float[] out) {
		MemorySegment s = shm;
		if (s == null || hostVersion < 17) {
			return false;
		}
		long b = OFF_HOST_PLAYER;
		for (int attempt = 0; attempt < 8; attempt++) {
			int seq1 = (int) INT.getAcquire(s, b + HPL_SEQ);
			if ((seq1 & 1) != 0 || seq1 == 0) {
				if (seq1 == 0) {
					return false;
				}
				Thread.onSpinWait();
				continue;
			}
			int set = s.get(JAVA_INT, b + HPL_HAND_LIGHT_SEQ);
			out[0] = s.get(JAVA_FLOAT, b + HPL_HAND_LIGHT);
			out[1] = s.get(JAVA_FLOAT, b + HPL_HAND_LIGHT + 4);
			out[2] = s.get(JAVA_FLOAT, b + HPL_HAND_LIGHT + 8);
			out[3] = s.get(JAVA_FLOAT, b + HPL_HAND_TONE);
			VarHandle.loadLoadFence();
			if ((int) INT.getAcquire(s, b + HPL_SEQ) == seq1) {
				return set != 0;
			}
		}
		return false;
	}

	/** Byte offset where the next HUD frame goes. */
	public static long hudBackSlotOffset() {
		return OFF_HUD_PIXELS + hudBack * HUD_SLOT_BYTES;
	}

	/** Publishes the HUD frame just written into the back slot. */
	public static void publishHud(int width, int height, long frameId, int guiScale, int mcFlags, int cameraMode) {
		MemorySegment s = shm;
		if (s == null || !hudAvailable()) {
			return;
		}
		long hdr = OFF_HUD_CTL + HC_SLOT + hudBack * HUD_SLOT_HDR_BYTES;
		s.set(JAVA_INT, hdr + HS_WIDTH, width);
		s.set(JAVA_INT, hdr + HS_HEIGHT, height);
		s.set(JAVA_INT, hdr + HS_FLAGS, 1); // bottom-up
		s.set(JAVA_INT, hdr + HS_GUI_SCALE, guiScale);
		s.set(JAVA_LONG, hdr + HS_FRAME_ID, frameId);
		s.set(JAVA_INT, hdr + HS_MC_FLAGS, mcFlags);
		s.set(JAVA_INT, hdr + HS_CAMERA_MODE, cameraMode);
		int old = (int) INT.getAndSet(s, OFF_HUD_CTL + HC_STATE, hudBack | OVERLAY_DIRTY);
		hudBack = old & 3;
		LONG.getAndAdd(s, OFF_HUD_CTL + HC_FRAMES_PUBLISHED, 1L);
	}

	// ---- collision ring (consume) ----------------------------------------------------------

	public static long collisionHead() {
		MemorySegment s = shm;
		return s == null ? 0 : (long) LONG.getAcquire(s, OFF_COLLISION_RING + CR_HEAD);
	}

	public static long collisionTail() {
		MemorySegment s = shm;
		return s == null ? 0 : s.get(JAVA_LONG, OFF_COLLISION_RING + CR_TAIL);
	}

	public static void setCollisionTail(long tail) {
		MemorySegment s = shm;
		if (s != null) {
			LONG.setRelease(s, OFF_COLLISION_RING + CR_TAIL, tail);
		}
	}
}
