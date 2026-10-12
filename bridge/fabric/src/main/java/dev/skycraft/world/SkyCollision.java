package dev.skycraft.world;

import static dev.skycraft.link.Proto.*;
import static java.lang.foreign.ValueLayout.*;

import dev.skycraft.SkyCraft;
import dev.skycraft.link.SkyLink;
import java.lang.foreign.MemorySegment;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;
import net.minecraft.core.BlockPos;
import net.minecraft.world.phys.shapes.BitSetDiscreteVoxelShape;
import net.minecraft.world.phys.shapes.CubeVoxelShape;
import net.minecraft.world.phys.shapes.Shapes;
import net.minecraft.world.phys.shapes.VoxelShape;
import org.jspecify.annotations.Nullable;

/**
 * Skyrim's world geometry as Minecraft sees it: an 8x8x8 sub-voxel collision shape per block
 * position, streamed from the SKSE plugin. These are not blocks; they are merged into block
 * collision queries (see BlockCollisionsMixin) so vanilla movement code collides with them.
 */
public final class SkyCollision {
	/** Skyrim regions are streamed as cubes of this many blocks. Must match the SKSE side. */
	public static final int REGION_SIZE = 8;

	private static final ConcurrentHashMap<Long, VoxelShape> SHAPES = new ConcurrentHashMap<>();
	// Per block: sub-voxel count (bits 0-9), any in the lower half (bit 10), any in the upper half (bit 11).
	private static final ConcurrentHashMap<Long, Integer> FILL = new ConcurrentHashMap<>();
	private static final int FILL_LOWER = 1 << 10;
	private static final int FILL_UPPER = 1 << 11;
	private static final int FILL_TOP_SHIFT = 12; // highest occupied of the 8 voxel layers (3 bits)
	private static final int FILL_NO_DIG = 1 << 15; // Portalcraft (bridge #33): ColBlock.pad had COL_BLOCK_NO_DIG (no host sets it since #38)
	private static final int FILL_LIMIT = 1 << 16; // Portalcraft (bridge #38): ColBlock.pad had COL_BLOCK_LIMIT (the cell is at the dig limit: bedrock)
	// Portalcraft (bridge #46): what the host says about how much of a cell is geometry you can SEE (ColBlock.pad bits 2, 3, 8..12); collision holds invisible brushes too
	private static final int FILL_VIS_KNOWN = 1 << 17;
	private static final int FILL_VIS_ANY = 1 << 18;
	private static final int FILL_VIS_SAMPLES_SHIFT = 19; // 5 bits
	// Portalcraft (bridge #33, digging): the raw 8 words of each cell (a shared FULL array for wholly solid cells), what the
	// dug-cell filter hides from every query above, and the regions that changed (walls of dug cells are rebuilt when they do)
	private static final long[] FULL_BITS = {-1L, -1L, -1L, -1L, -1L, -1L, -1L, -1L};
	private static final ConcurrentHashMap<Long, long[]> BITS = new ConcurrentHashMap<>();
	private static volatile java.util.function.LongPredicate dugFilter = k -> false;
	private static final java.util.concurrent.ConcurrentLinkedQueue<int[]> CHANGED = new java.util.concurrent.ConcurrentLinkedQueue<>();
	private static final java.util.concurrent.atomic.AtomicInteger CHANGED_COUNT = new java.util.concurrent.atomic.AtomicInteger();
	private static volatile boolean changedOverflow;
	private static volatile java.util.function.BooleanSupplier recordChanges = () -> false;
	private static final ConcurrentHashMap<Long, SkyTri[]> TRIS = new ConcurrentHashMap<>();
	private static volatile java.util.function.Predicate<net.minecraft.world.entity.Entity> smoothCollider = e -> false;
	private static final Set<Long> KNOWN_REGIONS = ConcurrentHashMap.newKeySet();
	private static volatile int epoch = -1;
	private static Thread consumer;

	private SkyCollision() {
	}

	public static @Nullable VoxelShape shapeAt(BlockPos pos) {
		// (bridge #49) the Minecraft stage is a room of the landscape: its edge is a wall for everything that moves (StageTerrain.barrier)
		if (pos.getY() >= StageTerrain.Y0 - 4 && StageTerrain.barrier(pos.getX(), pos.getY(), pos.getZ())) {
			return net.minecraft.world.phys.shapes.Shapes.block();
		}
		if (SHAPES.isEmpty()) {
			return null;
		}
		long key = pos.asLong();
		VoxelShape shape = SHAPES.get(key);
		if (shape == null || dugFilter.test(key)) {
			return null; // (bridge #33) dug cells have no host voxels in Minecraft
		}
		// (bridge #36) headroom over a dug cell: the host export's lowest sub-voxel row (5 u) of a cell directly above a dug one is no collision.
		// The export's 5 u rows put a floor up to 2.5 u above its true height and the 40 u cells start 8 u under the chamber floors, so a tunnel
		// dug 2 cells high (exactly a hull above the true floor) left the standing hull 2 u short of its ceiling and nothing could enter it.
		// The host's Chell collision (collfield.cpp CollisionFieldVoxelBoxes) drops the same row. Walls are drawn from the raw bits, unaffected.
		if (dugFilter.test(BlockPos.asLong(pos.getX(), pos.getY() - 1, pos.getZ()))) {
			return headroomShape(key, shape);
		}
		return shape;
	}

	private static final ConcurrentHashMap<Long, Object[]> HEADROOM = new ConcurrentHashMap<>(); // key -> {the raw words it was built from, shape or EMPTY_MARK}
	private static final Object NO_SHAPE = new Object();

	private static @Nullable VoxelShape headroomShape(long key, VoxelShape plain) {
		long[] bits = BITS.get(key);
		if (bits == null) {
			return plain;
		}
		Object[] cached = HEADROOM.get(key);
		if (cached != null && cached[0] == bits) {
			return cached[1] == NO_SHAPE ? null : (VoxelShape) cached[1];
		}
		long[] words = bits.clone();
		words[0] = 0L; // the lowest layer
		VoxelShape shape = shapeFromWords(words);
		HEADROOM.put(key, new Object[] {bits, shape == null ? NO_SHAPE : shape});
		return shape;
	}

	// ---- Portalcraft (bridge #33): digging -----------------------------------------------------------------

	/** SkyDig installs "this block position is dug in the current dig world": such cells' host voxels are gone in every query. */
	public static void setDugFilter(java.util.function.LongPredicate filter) {
		dugFilter = filter;
	}

	/** SkyDigClient installs "record changed regions" (true only while the host digs). */
	public static void setChangeRecorder(java.util.function.BooleanSupplier recorder) {
		recordChanges = recorder;
	}

	/** The cell's raw 8 voxel words (bits[y] bit z * 8 + x), dug or not; null if the host has nothing there. Do not modify. */
	public static long @Nullable [] rawBits(BlockPos pos) {
		return BITS.isEmpty() ? null : BITS.get(pos.asLong());
	}

	/** True if the host's ColBlock for this cell had COL_BLOCK_NO_DIG (dug or not). */
	public static boolean rawNoDig(BlockPos pos) {
		Integer fill = FILL.isEmpty() ? null : FILL.get(pos.asLong());
		return fill != null && (fill & FILL_NO_DIG) != 0;
	}

	/** True if the host's ColBlock for this cell had COL_BLOCK_LIMIT (bridge #38: at the dig limit; dug or not). */
	public static boolean rawLimit(BlockPos pos) {
		Integer fill = FILL.isEmpty() ? null : FILL.get(pos.asLong());
		return fill != null && (fill & FILL_LIMIT) != 0;
	}

	/**
	 * (bridge #46) The host has something to SEE in this cell (not only invisible brushes: clip, invisible walls, light blockers, which the export holds as
	 * collision so nobody walks through them). Digging and the crosshair treat the rest as air. A host that does not say (older) counts all content as visible.
	 */
	public static boolean rawHasVisible(BlockPos pos) {
		Integer fill = FILL.isEmpty() ? null : FILL.get(pos.asLong());
		return fill != null && ((fill & FILL_VIS_KNOWN) == 0 || (fill & FILL_VIS_ANY) != 0);
	}

	/** (bridge #46) The cell holds collision but nothing visible (see {@link #rawHasVisible}). */
	public static boolean rawGhostOnly(BlockPos pos) {
		Integer fill = FILL.isEmpty() ? null : FILL.get(pos.asLong());
		return fill != null && (fill & FILL_VIS_KNOWN) != 0 && (fill & FILL_VIS_ANY) == 0;
	}

	/**
	 * (bridge #46) How many of the cell's 27 reveal sample points (DigMath.samples) are VISIBLE solid: the host's count when it gives one, else the raw bits' count
	 * (an older host), -1 if the region is not here.
	 */
	public static int visibleSamples(BlockPos pos) {
		Integer fill = FILL.isEmpty() ? null : FILL.get(pos.asLong());
		if (fill == null) {
			return DigMath.samples(rawBits(pos));
		}
		return (fill & FILL_VIS_KNOWN) != 0 ? (fill >>> FILL_VIS_SAMPLES_SHIFT) & 31 : DigMath.samples(rawBits(pos));
	}

	/** True if the host has content in this cell, dug or not. */
	public static boolean rawHasGeometry(BlockPos pos) {
		return !FILL.isEmpty() && FILL.containsKey(pos.asLong());
	}

	/** Takes the regions (min x, y, z, max x, y, z in blocks) the host sent since the last call; returns false if some were dropped (everything changed). */
	public static boolean takeChangedRegions(java.util.function.Consumer<int[]> out) {
		int[] r;
		while ((r = CHANGED.poll()) != null) {
			CHANGED_COUNT.decrementAndGet();
			out.accept(r);
		}
		boolean ok = !changedOverflow;
		changedOverflow = false;
		return ok;
	}

	private static void noteChanged(int minX, int minY, int minZ, int maxX, int maxY, int maxZ) {
		if (!recordChanges.getAsBoolean()) {
			return;
		}
		if (CHANGED_COUNT.get() > 4096) {
			CHANGED.clear();
			CHANGED_COUNT.set(0);
			changedOverflow = true;
			return;
		}
		CHANGED.add(new int[] {minX, minY, minZ, maxX, maxY, maxZ});
		CHANGED_COUNT.incrementAndGet();
	}

	/** Entities (the local player) that collide with Skyrim's exact triangles instead of its voxels. */
	public static void setSmoothCollider(java.util.function.Predicate<net.minecraft.world.entity.Entity> predicate) {
		smoothCollider = predicate;
	}

	public static boolean usesSmoothCollider(net.minecraft.world.entity.@Nullable Entity entity) {
		return entity != null && smoothCollider.test(entity);
	}

	/** Adds every Skyrim triangle whose bounds overlap {@code box}. */
	public static void trianglesNear(net.minecraft.world.phys.AABB box, java.util.List<SkyTri> out) {
		if (TRIS.isEmpty()) {
			return;
		}
		int rx0 = Math.floorDiv((int) Math.floor(box.minX), REGION_SIZE), rx1 = Math.floorDiv((int) Math.floor(box.maxX), REGION_SIZE);
		int ry0 = Math.floorDiv((int) Math.floor(box.minY), REGION_SIZE), ry1 = Math.floorDiv((int) Math.floor(box.maxY), REGION_SIZE);
		int rz0 = Math.floorDiv((int) Math.floor(box.minZ), REGION_SIZE), rz1 = Math.floorDiv((int) Math.floor(box.maxZ), REGION_SIZE);
		for (int rx = rx0; rx <= rx1; rx++) {
			for (int ry = ry0; ry <= ry1; ry++) {
				for (int rz = rz0; rz <= rz1; rz++) {
					SkyTri[] tris = TRIS.get(regionKey(rx, ry, rz));
					if (tris == null) {
						continue;
					}
					for (SkyTri t : tris) {
						if (t.maxX >= box.minX && t.minX <= box.maxX && t.maxY >= box.minY && t.minY <= box.maxY && t.maxZ >= box.minZ && t.minZ <= box.maxZ) {
							out.add(t);
						}
					}
				}
			}
		}
	}

	/** True once Skyrim has sent the region containing this block (even if it was empty). */
	public static boolean isKnown(int x, int y, int z) {
		return KNOWN_REGIONS.contains(regionKey(Math.floorDiv(x, REGION_SIZE), Math.floorDiv(y, REGION_SIZE), Math.floorDiv(z, REGION_SIZE)));
	}

	/** True if any Skyrim geometry exists in the 3x3 column below (x, y, z), down to {@code depth} blocks. */
	public static boolean hasSolidBelow(int x, int y, int z, int depth) {
		for (int dy = 0; dy <= depth; dy++) {
			for (int dx = -1; dx <= 1; dx++) {
				for (int dz = -1; dz <= 1; dz++) {
					long k = BlockPos.asLong(x + dx, y - dy, z + dz);
					if (SHAPES.containsKey(k) && !dugFilter.test(k)) {
						return true;
					}
				}
			}
		}
		return false;
	}

	/** Fraction (0..1) of this block's volume that is Skyrim geometry. */
	public static float solidFraction(BlockPos pos) {
		Integer fill = fillAt(pos.asLong());
		return fill == null ? 0.0F : (fill & 0x3FF) / 512.0F;
	}

	/** True if any Skyrim geometry is in this cell. */
	public static boolean hasGeometry(BlockPos pos) {
		return fillAt(pos.asLong()) != null;
	}

	/** The cell's fill info unless it is dug (bridge #33). */
	private static @Nullable Integer fillAt(long key) {
		Integer fill = FILL.isEmpty() ? null : FILL.get(key);
		return fill != null && dugFilter.test(key) ? null : fill;
	}

	/**
	 * How high (0..1) Skyrim geometry reaches in this cell: the top of its highest part. Terrain
	 * arrives as a thin surface, so what lies below that surface counts as ground too.
	 */
	public static float groundTop(BlockPos pos) {
		Integer fill = fillAt(pos.asLong());
		return fill == null ? 0.0F : (((fill >> FILL_TOP_SHIFT) & 7) + 1) / 8.0F;
	}

	/** True if Skyrim ground holds up whatever is in this cell (terrain in its lower half or the top of the cell below). */
	public static boolean supportsFromBelow(BlockPos pos) {
		if (FILL.isEmpty()) {
			return false;
		}
		Integer here = fillAt(pos.asLong());
		if (here != null && (here & FILL_LOWER) != 0) {
			return true;
		}
		Integer below = fillAt(BlockPos.asLong(pos.getX(), pos.getY() - 1, pos.getZ()));
		return below != null && (below & FILL_UPPER) != 0;
	}

	public static int blockCount() {
		return SHAPES.size();
	}

	public static int regionCount() {
		return KNOWN_REGIONS.size();
	}

	/** Skyrim is describing its world around the player (false in a plain Minecraft world). */
	public static boolean active() {
		return !KNOWN_REGIONS.isEmpty();
	}

	private static long regionKey(int rx, int ry, int rz) {
		return BlockPos.asLong(rx, ry, rz);
	}

	public static synchronized void startConsumer() {
		if (consumer != null) {
			return;
		}
		consumer = new Thread(SkyCollision::consumeLoop, "Portalcraft collision");
		consumer.setDaemon(true);
		consumer.start();
	}

	private static void consumeLoop() {
		while (true) {
			try {
				if (!drainOnce()) {
					Thread.sleep(2);
				}
			} catch (InterruptedException e) {
				return;
			} catch (Throwable t) {
				SkyCraft.LOG.error("SkyCraft: collision consumer error", t);
				try {
					Thread.sleep(500);
				} catch (InterruptedException e) {
					return;
				}
			}
		}
	}

	/** Processes all pending collision messages. Returns true if anything was consumed. */
	private static boolean drainOnce() {
		MemorySegment s = SkyLink.segment();
		if (s == null) {
			return false;
		}
		long head = SkyLink.collisionHead();
		long tail = SkyLink.collisionTail();
		if (tail >= head) {
			return false;
		}
		long data = OFF_COLLISION_RING + CR_DATA;
		while (tail < head) {
			long pos = tail % CR_DATA_BYTES;
			int type = s.get(JAVA_INT, data + pos);
			int payloadBytes = s.get(JAVA_INT, data + pos + 4);
			if (type == COL_PAD) {
				tail += CR_DATA_BYTES - pos;
				continue;
			}
			long payload = data + pos + 8;
			switch (type) {
				case COL_CLEAR -> clear(s.get(JAVA_INT, payload));
				case COL_REGION -> readRegion(s, payload);
				case COL_TRIS -> readTris(s, payload);
				case COL_MOVER_SHAPE -> SkyMovers.readShape(s, payload); // Portalcraft v18 (bridge #26)
				case COL_MOVER_BIND -> SkyMovers.readBind(s, payload);
				case COL_MOVER_LATTICE -> SkyMovers.readLattice(s, payload); // bridge #43: a mover shape's dig lattice
				default -> SkyCraft.LOG.warn("SkyCraft: unknown collision message {}", type);
			}
			tail += align8(8 + payloadBytes);
		}
		SkyLink.setCollisionTail(tail);
		return true;
	}

	private static long align8(long v) {
		return (v + 7) & ~7L;
	}

	/** A freshly started client joins whatever collision epoch Skyrim is already on. */
	private static void adoptEpochIfFresh(int msgEpoch) {
		if (epoch == -1) {
			epoch = msgEpoch;
			SkyCraft.LOG.info("SkyCraft: joined collision epoch {} already in progress", msgEpoch);
		}
	}

	private static void clear(int newEpoch) {
		SHAPES.clear();
		FILL.clear();
		BITS.clear();
		HEADROOM.clear();
		noteChanged(Integer.MIN_VALUE / 2, Integer.MIN_VALUE / 2, Integer.MIN_VALUE / 2, Integer.MAX_VALUE / 2, Integer.MAX_VALUE / 2, Integer.MAX_VALUE / 2);
		TRIS.clear();
		KNOWN_REGIONS.clear();
		SkyMovers.clear();
		epoch = newEpoch;
		SkyCraft.LOG.info("SkyCraft: collision cleared (epoch {})", newEpoch);
	}

	private static void readRegion(MemorySegment s, long p) {
		int minX = s.get(JAVA_INT, p);
		int minY = s.get(JAVA_INT, p + 4);
		int minZ = s.get(JAVA_INT, p + 8);
		int maxX = s.get(JAVA_INT, p + 12);
		int maxY = s.get(JAVA_INT, p + 16);
		int maxZ = s.get(JAVA_INT, p + 20);
		int msgEpoch = s.get(JAVA_INT, p + 24);
		int count = s.get(JAVA_INT, p + 28);
		adoptEpochIfFresh(msgEpoch);
		if (msgEpoch != epoch) {
			return; // stale region from before a world change
		}

		// Build the new shapes first so readers never see a half-empty region.
		java.util.HashMap<Long, VoxelShape> fresh = new java.util.HashMap<>(count * 2);
		java.util.HashMap<Long, Integer> freshFill = new java.util.HashMap<>(count * 2);
		java.util.HashMap<Long, long[]> freshBits = new java.util.HashMap<>(count * 2);
		long e = p + COL_REGION_HEADER_BYTES;
		for (int i = 0; i < count; i++, e += COL_BLOCK_BYTES) {
			int x = s.get(JAVA_INT, e);
			int y = s.get(JAVA_INT, e + 4);
			int z = s.get(JAVA_INT, e + 8);
			VoxelShape shape = buildShape(s, e + 16);
			if (shape != null) {
				long key = BlockPos.asLong(x, y, z);
				fresh.put(key, shape);
				int pad = s.get(JAVA_INT, e + 12);
				freshFill.put(key, fillInfo(s, e + 16) | ((pad & COL_BLOCK_NO_DIG) != 0 ? FILL_NO_DIG : 0) | ((pad & COL_BLOCK_LIMIT) != 0 ? FILL_LIMIT : 0) | visFill(pad));
				freshBits.put(key, rawWords(s, e + 16));
			}
		}

		for (int x = minX; x <= maxX; x++) {
			for (int y = minY; y <= maxY; y++) {
				for (int z = minZ; z <= maxZ; z++) {
					long key = BlockPos.asLong(x, y, z);
					VoxelShape shape = fresh.get(key);
					if (shape != null) {
						SHAPES.put(key, shape);
						FILL.put(key, freshFill.get(key));
						BITS.put(key, freshBits.get(key));
					} else {
						SHAPES.remove(key);
						FILL.remove(key);
						BITS.remove(key);
					}
				}
			}
		}

		for (int rx = Math.floorDiv(minX, REGION_SIZE); rx <= Math.floorDiv(maxX, REGION_SIZE); rx++) {
			for (int ry = Math.floorDiv(minY, REGION_SIZE); ry <= Math.floorDiv(maxY, REGION_SIZE); ry++) {
				for (int rz = Math.floorDiv(minZ, REGION_SIZE); rz <= Math.floorDiv(maxZ, REGION_SIZE); rz++) {
					KNOWN_REGIONS.add(regionKey(rx, ry, rz));
				}
			}
		}
		noteChanged(minX, minY, minZ, maxX, maxY, maxZ); // (bridge #33) walls of dug cells next to this region are rebuilt
	}

	/** (bridge #46) The FILL bits for a ColBlock.pad's visibility fields (nothing for an older host: the whole cell counts as visible). */
	static int visFill(int pad) {
		if ((pad & COL_BLOCK_VIS_KNOWN) == 0) {
			return 0;
		}
		return FILL_VIS_KNOWN | ((pad & COL_BLOCK_VIS_ANY) != 0 ? FILL_VIS_ANY : 0) | (((pad & COL_BLOCK_VIS_SAMPLES_MASK) >>> COL_BLOCK_VIS_SAMPLES_SHIFT) << FILL_VIS_SAMPLES_SHIFT);
	}

	/** A cell's 8 words; wholly solid cells share one array. */
	private static long[] rawWords(MemorySegment s, long bitsOff) {
		long[] w = new long[8];
		boolean full = true;
		for (int y = 0; y < 8; y++) {
			w[y] = s.get(JAVA_LONG, bitsOff + y * 8L);
			full &= w[y] == -1L;
		}
		return full ? FULL_BITS : w;
	}

	private static void readTris(MemorySegment s, long p) {
		int minX = s.get(JAVA_INT, p);
		int minY = s.get(JAVA_INT, p + 4);
		int minZ = s.get(JAVA_INT, p + 8);
		int msgEpoch = s.get(JAVA_INT, p + 24);
		int count = s.get(JAVA_INT, p + 28);
		adoptEpochIfFresh(msgEpoch);
		if (msgEpoch != epoch) {
			return;
		}
		SkyTri[] tris = new SkyTri[count];
		float[] v = new float[9];
		int kept = 0;
		long e = p + COL_REGION_HEADER_BYTES;
		for (int i = 0; i < count; i++, e += COL_TRI_BYTES) {
			for (int k = 0; k < 9; k++) {
				v[k] = s.get(JAVA_FLOAT, e + k * 4L);
			}
			int flags = s.get(JAVA_INT, e + 36);
			SkyTri t = new SkyTri(v, 0, (flags & TRI_STAIR_HELPER) != 0);
			if (!t.degenerate()) {
				tris[kept++] = t;
			}
		}
		TRIS.put(regionKey(Math.floorDiv(minX, REGION_SIZE), Math.floorDiv(minY, REGION_SIZE), Math.floorDiv(minZ, REGION_SIZE)), java.util.Arrays.copyOf(tris, kept));
	}

	public static int triangleCount() {
		int n = 0;
		for (SkyTri[] t : TRIS.values()) {
			n += t.length;
		}
		return n;
	}

	private static int fillInfo(MemorySegment s, long bitsOff) {
		int count = 0;
		int info = 0;
		int top = 0;
		for (int y = 0; y < 8; y++) {
			long layer = s.get(JAVA_LONG, bitsOff + y * 8L);
			count += Long.bitCount(layer);
			if (layer != 0) {
				info |= y < 4 ? FILL_LOWER : FILL_UPPER;
				top = y;
			}
		}
		return info | count | top << FILL_TOP_SHIFT;
	}

	private static @Nullable VoxelShape buildShape(MemorySegment s, long bitsOff) {
		long[] layers = new long[8];
		for (int y = 0; y < 8; y++) {
			layers[y] = s.get(JAVA_LONG, bitsOff + y * 8L);
		}
		return shapeFromWords(layers);
	}

	private static @Nullable VoxelShape shapeFromWords(long[] layers) {
		boolean any = false;
		boolean full = true;
		for (int y = 0; y < 8; y++) {
			any |= layers[y] != 0;
			full &= layers[y] == -1L;
		}
		if (!any) {
			return null;
		}
		if (full) {
			return Shapes.block();
		}
		BitSetDiscreteVoxelShape discrete = new BitSetDiscreteVoxelShape(8, 8, 8);
		for (int y = 0; y < 8; y++) {
			long layer = layers[y];
			while (layer != 0) {
				int bit = Long.numberOfTrailingZeros(layer);
				layer &= layer - 1;
				discrete.fill(bit & 7, y, bit >>> 3);
			}
		}
		return new CubeVoxelShape(discrete);
	}

	/** (bridge #53) the sizes of this class's static caches and queues, for the periodic growth line in the log. */
	public static String growth() {
		return "collision shapes " + SHAPES.size() + ", fill " + FILL.size() + ", bits " + BITS.size() + ", tris " + TRIS.size() + ", headroom " + HEADROOM.size() + ", known regions " + KNOWN_REGIONS.size() + ", changed " + CHANGED_COUNT.get();
	}
}
