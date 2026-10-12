package dev.skycraft.world;

import dev.skycraft.SkyCraft;
import java.util.EnumSet;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import net.fabricmc.fabric.api.event.lifecycle.v1.ServerChunkEvents;
import net.fabricmc.fabric.api.event.lifecycle.v1.ServerTickEvents;
import net.minecraft.core.BlockPos;
import net.minecraft.core.SectionPos;
import net.minecraft.network.protocol.game.ClientboundLevelChunkWithLightPacket;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.level.ChunkPos;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.block.LeavesBlock;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.chunk.LevelChunk;
import net.minecraft.world.level.chunk.LevelChunkSection;
import net.minecraft.world.level.levelgen.Heightmap;

/**
 * Portalcraft (bridge #47): the Minecraft STAGE, real terrain in the mirror world for the portal that opens onto Minecraft in every
 * campaign chamber.
 *
 * <p>The mirror world is a void: Portal 2's chamber is exported into it at the host's own coordinates (block = 40 u, y up). A
 * portal fired with G needs somewhere in Minecraft to open onto, in every chamber, without a hidden box in the map (the campaign maps
 * are Valve's, nothing is added to them). So the stage is one landscape, the same for every map, that sits high above everything
 * the chambers use (blocks y 640..767; Portal 2's world is +-16384 u = +-410 blocks), in the same chunk columns as the chamber, so
 * Minecraft keeps it loaded wherever the player is. The host shows it through the portal by mapping a window of it onto an empty
 * box of its own world (bridge/src/bridge/stage.cpp).
 *
 * <p>The landscape is made so a cliff exit always exists: wide plateaus and lowlands in strips along x, 128 blocks across, joined by
 * vertical cliff faces 24 blocks tall (they step sideways by 0..3 blocks every 24 blocks along z, so each run is a flat face), with
 * rolling grass, lakes and a few oaks on either side. The wall at x = k * 256 faces -X (stone behind, open air in front, the lowland
 * at its foot): the exit.
 *
 * <p>It is only made where the host asks (HP_STAGE: the chunk and the radius around it, StageHost), in the chunks that are already
 * loaded (and re-sent), and in the ones that load later. A world that never opens a portal onto Minecraft never has one. It is
 * saved with the world, and a chunk that already has it (bedrock at y 640) is left as it is.
 *
 * <p>Keep the constants in step with bridge/src/bridge/stage.cpp.
 */
public final class StageTerrain {
	private StageTerrain() {
	}

	public static final int Y0 = 640;      // the bedrock layer
	public static final int LOW = 704;     // the lowlands' ground level (the surface's top block)
	public static final int WALL_H = 24;   // the plateaus stand this much above it
	public static final int SEA = 699;     // water level: lake basins are filled up to here
	public static final int PERIOD = 256;  // one plateau + one lowland
	public static final int HALF = 128;    // the plateau part of it: x in [k * 256, k * 256 + 128) (+ the run's offset)
	public static final int RUN = 24;      // a cliff face's runs along z
	public static final int TOP = 768;     // the stage fills y < TOP
	public static final int SEC_MIN = Y0 >> 4, SEC_MAX = (TOP - 1) >> 4;

	private static final int SEED = 0x5ca1e;

	// ---- the landscape (pure functions of x, z: testable without Minecraft) -----------------------------------

	private static int hash(int x, int z, int seed) {
		int h = x * 374761393 + z * 668265263 + seed * 1442695041;
		h = (h ^ (h >>> 13)) * 1274126177;
		return h ^ (h >>> 16);
	}

	private static double rnd01(int x, int z, int seed) {
		return (hash(x, z, seed) & 0xFFFFFF) / (double) 0x1000000;
	}

	private static double smooth(double t) {
		return t * t * (3 - 2 * t);
	}

	private static double vnoise(double x, double z, int seed) {
		int x0 = (int) Math.floor(x), z0 = (int) Math.floor(z);
		double fx = smooth(x - x0), fz = smooth(z - z0);
		double a = rnd01(x0, z0, seed), b = rnd01(x0 + 1, z0, seed), c = rnd01(x0, z0 + 1, seed), d = rnd01(x0 + 1, z0 + 1, seed);
		return (a + (b - a) * fx) + ((c + (d - c) * fx) - (a + (b - a) * fx)) * fz;
	}

	/** Rolling noise in -1..1 (three octaves, 48 blocks and down). */
	static double fbm(int x, int z) {
		double v = 0, amp = 1, norm = 0, scale = 1.0 / 48.0;
		for (int i = 0; i < 3; i++) {
			v += amp * (vnoise(x * scale, z * scale, SEED + i * 131) * 2 - 1);
			norm += amp;
			amp *= 0.5;
			scale *= 2.1;
		}
		return v / norm;
	}

	/** How far (blocks) the cliff faces of this run of z are shifted from x = k * 256: 0..3. */
	public static int offset(int z) {
		return hash(Math.floorDiv(z, RUN), 77, SEED) & 3;
	}

	/** x is on a plateau (else lowland). */
	public static boolean plateau(int x, int z) {
		return Math.floorMod(x - offset(z), PERIOD) < HALF;
	}

	/** Blocks to the nearest cliff face along x (0 = the first block of a plateau at a face, 1 = the lowland block right in front of one). */
	public static int distToWall(int x, int z) {
		int u = Math.floorMod(x - offset(z), PERIOD);
		return Math.min(Math.min(u, PERIOD - u), Math.abs(u - HALF));
	}

	/** The top solid block of the column (the surface). Water fills above it up to SEA. */
	public static int surface(int x, int z) {
		boolean hi = plateau(x, z);
		int dw = distToWall(x, z);
		double flat = Math.min(1.0, 0.12 + dw / 28.0);
		double n = fbm(x, z);
		int h = (hi ? LOW + WALL_H : LOW) + (int) Math.round(n * (hi ? 7 : 5) * flat);
		if (!hi && dw > 20) {
			double lake = vnoise(x / 64.0, z / 64.0, SEED + 500);  // a lake basin (water from lake > 0.75)
			if (lake > 0.7) {
				h = Math.min(h, LOW - 2 - (int) ((lake - 0.7) * 60));
			}
		}
		return h;
	}

	/** The column is a beach (sand, no grass): the surface is at the water's edge. */
	public static boolean sandy(int x, int z) {
		return surface(x, z) <= SEA + 1;
	}

	/** An oak stands on this column (its trunk's foot): grass, dry, away from the cliffs. */
	public static boolean treeAt(int x, int z) {
		return !sandy(x, z) && surface(x, z) > SEA + 2 && distToWall(x, z) > 8 && rnd01(x, z, SEED + 99) < 1.0 / 150.0;
	}

	/** Trunk height 4..5 of the tree at this column. */
	public static int treeHeight(int x, int z) {
		return 4 + (hash(x, z, SEED + 5) & 1);
	}

	/** The block of the landscape at x y z as a code: 0 air, 1 bedrock, 2 stone, 3 dirt, 4 grass, 5 sand, 6 water (no trees). */
	public static int code(int x, int y, int z) {
		if (y < Y0 || y >= TOP) {
			return 0;
		}
		if (y == Y0) {
			return 1;
		}
		int s = surface(x, z);
		if (y > s) {
			return y <= SEA ? 6 : 0;
		}
		boolean sand = s <= SEA + 1;
		if (y == s) {
			return sand ? 5 : 4;
		}
		if (y > s - 4) {
			return sand ? 5 : 3;
		}
		return 2;
	}

	// ---- worlds ------------------------------------------------------------------------------------------------

	/** The section y is in the stage's height range. */
	public static boolean sectionInStage(int sy) {
		return sy >= SEC_MIN && sy <= SEC_MAX;
	}

	/** This is the mirror world, where the host can ask for the stage (the terrain world has real terrain and none). */
	public static boolean active() {
		return registered;
	}

	/** y is inside the stage's height range. */
	public static boolean inStage(double y) {
		return registered && y >= Y0 - 8 && y < TOP + 8;  // (the mirror world only)
	}

	/** How far (blocks) from the window's centre chunk the stage is walled in: Chell's room in the host is 80 blocks each way around the window's centre. */
	public static final int ROOM = 72;

	/**
	 * (bridge #49) The stage's edge: the host shows a window of the stage as a box of its own world (Chell cannot leave it), and the Minecraft
	 * player is given the same limits. A cell of the stage's height range (above the world's end too) outside the window around the host's
	 * centre chunk is a solid wall to anything that moves (SkyCollision.shapeAt). Only while the host asks for the stage.
	 */
	public static boolean barrier(int x, int y, int z) {
		if (!registered || y >= TOP + 16 || !StageHost.enabled()) {
			return false;
		}
		int cx = StageHost.chunkX() * 16 + 8, cz = StageHost.chunkZ() * 16 + 8;
		return Math.abs(x - cx) > ROOM || Math.abs(z - cz) > ROOM || y >= TOP;
	}

	private static boolean registered;
	private static int lastLog;
	private static final Set<Long> RESENT = new HashSet<>();

	/** Mirror world only (the terrain world has real terrain and no chambers over it). */
	public static synchronized void init() {
		if (registered || "terrain".equals(System.getProperty("skycraft.world", "mirror")) || "false".equals(System.getProperty("portalcraft.stage"))) {
			return;
		}
		registered = true;
		ServerChunkEvents.CHUNK_LOAD.register((level, chunk, generated) -> {
			if (StageHost.enabled() && wanted(chunk.getPos()) && !filled(chunk)) {
				fill(level, chunk, false);
			}
		});
		ServerTickEvents.END_SERVER_TICK.register(StageTerrain::tick);
		SkyCraft.LOG.info("Portalcraft stage: ready (made on the host's request: blocks y {}..{}, cliff walls at x = k * {})", Y0, TOP - 1, PERIOD);
	}

	private static boolean wanted(ChunkPos pos) {
		int r = StageHost.radius();
		return Math.abs(pos.x() - StageHost.chunkX()) <= r && Math.abs(pos.z() - StageHost.chunkZ()) <= r;
	}

	private static boolean filled(LevelChunk chunk) {
		ChunkPos p = chunk.getPos();
		return chunk.getBlockState(new BlockPos(p.getMinBlockX(), Y0, p.getMinBlockZ())).is(Blocks.BEDROCK);
	}

	private static int tickCount;

	private static void tick(MinecraftServer server) {
		if (++tickCount % 4 != 0 || !StageHost.enabled()) {
			return;
		}
		ServerLevel level = server.overworld();
		if (level == null) {
			return;
		}
		int cx = StageHost.chunkX(), cz = StageHost.chunkZ(), r = StageHost.radius();
		int budget = 6;
		// the chunks that are loaded already (nearest the window first) get it now, and go to the players again
		for (int ring = 0; ring <= r && budget > 0; ring++) {
			for (int dx = -ring; dx <= ring && budget > 0; dx++) {
				for (int dz = -ring; dz <= ring && budget > 0; dz++) {
					if (Math.max(Math.abs(dx), Math.abs(dz)) != ring) {
						continue;
					}
					LevelChunk chunk = level.getChunkSource().getChunkNow(cx + dx, cz + dz);
					if (chunk == null || filled(chunk)) {
						continue;
					}
					fill(level, chunk, true);
					budget--;
				}
			}
		}
	}

	/** Makes the stage in this chunk (every column, y 640..767). */
	static void fill(ServerLevel level, LevelChunk chunk, boolean resend) {
		long t0 = System.nanoTime();
		ChunkPos pos = chunk.getPos();
		int x0 = pos.getMinBlockX(), z0 = pos.getMinBlockZ();
		int[] surf = new int[256];
		boolean[] sand = new boolean[256];
		int maxSurf = 0;
		for (int lz = 0; lz < 16; lz++) {
			for (int lx = 0; lx < 16; lx++) {
				int s = surface(x0 + lx, z0 + lz);
				surf[lz * 16 + lx] = s;
				sand[lz * 16 + lx] = s <= SEA + 1;
				maxSurf = Math.max(maxSurf, s);
			}
		}
		BlockState stone = Blocks.STONE.defaultBlockState(), dirt = Blocks.DIRT.defaultBlockState(), grass = Blocks.GRASS_BLOCK.defaultBlockState(),
			sandS = Blocks.SAND.defaultBlockState(), water = Blocks.WATER.defaultBlockState(), bedrock = Blocks.BEDROCK.defaultBlockState();
		int top = Math.min(TOP, Math.max(maxSurf + 1, SEA + 1));
		int blocks = 0;
		for (int sy = SEC_MIN; sy <= SEC_MAX; sy++) {
			if (sy * 16 >= top) {
				break;
			}
			LevelChunkSection section = chunk.getSection(chunk.getSectionIndexFromSectionY(sy));
			for (int ly = 0; ly < 16; ly++) {
				int y = sy * 16 + ly;
				if (y >= top) {
					break;
				}
				for (int lz = 0; lz < 16; lz++) {
					for (int lx = 0; lx < 16; lx++) {
						int s = surf[lz * 16 + lx];
						BlockState st;
						if (y == Y0) {
							st = bedrock;
						} else if (y > s) {
							if (y > SEA) {
								continue;
							}
							st = water;
						} else if (y == s) {
							st = sand[lz * 16 + lx] ? sandS : grass;
						} else if (y > s - 4) {
							st = sand[lz * 16 + lx] ? sandS : dirt;
						} else {
							st = stone;
						}
						section.setBlockState(lx, ly, lz, st, false);
						blocks++;
					}
				}
			}
		}
		// the oaks (all inside the chunk: a trunk 3..12 blocks in from every edge, the canopy reaches 2 out)
		BlockState log = Blocks.OAK_LOG.defaultBlockState(), leaves = Blocks.OAK_LEAVES.defaultBlockState().setValue(LeavesBlock.PERSISTENT, true);
		int trees = 0;
		for (int lz = 3; lz < 13; lz++) {
			for (int lx = 3; lx < 13; lx++) {
				int x = x0 + lx, z = z0 + lz;
				if (!treeAt(x, z)) {
					continue;
				}
				int s = surf[lz * 16 + lx], h = treeHeight(x, z);
				for (int dy = h - 1; dy <= h + 2; dy++) {
					int reach = dy <= h ? 2 : 1;
					for (int dz = -reach; dz <= reach; dz++) {
						for (int dx = -reach; dx <= reach; dx++) {
							if (reach == 2 && Math.abs(dx) == 2 && Math.abs(dz) == 2) {
								continue;  // no corners
							}
							if (dx == 0 && dz == 0 && dy <= h) {
								continue;  // the trunk
							}
							setLocal(chunk, lx + dx, s + dy, lz + dz, leaves);
						}
					}
				}
				for (int dy = 1; dy <= h; dy++) {
					setLocal(chunk, lx, s + dy, lz, log);
				}
				trees++;
			}
		}
		// the heightmaps, the light, the save, and the players who already have the chunk
		Set<Heightmap.Types> types = EnumSet.noneOf(Heightmap.Types.class);
		chunk.getHeightmaps().forEach(e -> types.add(e.getKey()));
		Heightmap.primeHeightmaps(chunk, types);
		chunk.markUnsaved();
		var light = level.getChunkSource().getLightEngine();
		for (int sy = SEC_MIN; sy <= SEC_MAX; sy++) {
			light.updateSectionStatus(SectionPos.of(pos.x(), sy, pos.z()), chunk.getSection(chunk.getSectionIndexFromSectionY(sy)).hasOnlyAir());
		}
		if (resend) {
			resend(level, chunk);  // at once (the picture first; the light follows below, and the chunk goes again when it is in)
		}
		try {
			final var engine = light;
			engine.initializeLight(chunk, false).thenCompose(c -> engine.lightChunk(c, false)).thenAcceptAsync(c -> {
				if (resend) {
					resend(level, chunk);
				}
			}, level.getServer()).exceptionally(t -> {
				if (lastLog++ < 5) {
					SkyCraft.LOG.warn("Portalcraft stage: the light of chunk {} {} failed: {}", pos.x(), pos.z(), t.toString());
				}
				return null;
			});
		} catch (RuntimeException e) {
			if (lastLog++ < 5) {
				SkyCraft.LOG.warn("Portalcraft stage: could not relight chunk {} {}: {}", pos.x(), pos.z(), e.toString());
			}
		}
		if (lastLog++ < 12) {
			SkyCraft.LOG.info("Portalcraft stage: chunk {} {} made ({} blocks, {} oaks, {} ms{})", pos.x(), pos.z(), blocks, trees, String.format("%.1f", (System.nanoTime() - t0) / 1e6), resend ? ", sent again" : "");
		}
	}

	private static void setLocal(LevelChunk chunk, int lx, int y, int lz, BlockState state) {
		if (lx < 0 || lx > 15 || lz < 0 || lz > 15 || y < Y0 || y >= TOP) {
			return;
		}
		LevelChunkSection section = chunk.getSection(chunk.getSectionIndexFromSectionY(y >> 4));
		BlockState old = section.getBlockState(lx, y & 15, lz);
		if (old.isAir() || old.is(Blocks.WATER)) {
			section.setBlockState(lx, y & 15, lz, state, false);
		}
	}

	private static void resend(ServerLevel level, LevelChunk chunk) {
		List<ServerPlayer> players = level.getChunkSource().chunkMap.getPlayers(chunk.getPos(), false);
		for (ServerPlayer p : players) {
			p.connection.send(new ClientboundLevelChunkWithLightPacket(chunk, level.getLightEngine(), null, null));
		}
		synchronized (RESENT) {
			RESENT.add(chunk.getPos().pack());
		}
	}
}
