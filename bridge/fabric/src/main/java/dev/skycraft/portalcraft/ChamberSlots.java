package dev.skycraft.portalcraft;

import dev.skycraft.SkyCraft;
import dev.skycraft.link.SkyLink;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import net.fabricmc.fabric.api.event.lifecycle.v1.ServerChunkEvents;
import net.fabricmc.fabric.api.event.lifecycle.v1.ServerEntityEvents;
import net.fabricmc.fabric.api.event.lifecycle.v1.ServerLifecycleEvents;
import net.fabricmc.fabric.api.event.lifecycle.v1.ServerTickEvents;
import net.minecraft.core.BlockPos;
import net.minecraft.core.registries.Registries;
import net.minecraft.nbt.CompoundTag;
import net.minecraft.nbt.ListTag;
import net.minecraft.nbt.NbtAccounter;
import net.minecraft.nbt.NbtIo;
import net.minecraft.nbt.NbtUtils;
import net.minecraft.nbt.Tag;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.util.ProblemReporter;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.entity.EntitySpawnReason;
import net.minecraft.world.entity.EntityType;
import net.minecraft.world.entity.player.Player;
import net.minecraft.world.level.ChunkPos;
import net.minecraft.world.level.Level;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.block.entity.BlockEntity;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.chunk.LevelChunk;
import net.minecraft.world.level.chunk.LevelChunkSection;
import net.minecraft.world.level.storage.LevelResource;
import net.minecraft.world.level.storage.TagValueInput;
import net.minecraft.world.level.storage.TagValueOutput;

/**
 * Portalcraft (bridge #78): every map's chamber keeps its own Minecraft blocks in the unified (terrain) world.
 *
 * <p>The host maps every chamber of every map into the same CHAMBER BAND of this world (Minecraft y 320 and up: vanilla's generator makes
 * nothing there; the plugin's unified.cpp puts Portal 2's whole world at y 342..1162 over the pinned cliff). What the player builds in a
 * chamber, and the blocks digging reveals (SkyDig), are real blocks of this one world, so without this the next map found the last map's
 * blocks floating in its chambers (the user's "blocks floating about"). Here they belong to the map that was loaded when they were made:
 * <ul>
 * <li>when the host names another map (SkyState worldId, a hash of the map name), the band's blocks (block entities with their contents)
 * and the non-player entities in it are stored under the old map, removed, and the new map's stored ones are put back, in every loaded
 * chunk;</li>
 * <li>a chunk that loads later is swapped as it loads: it holds the blocks of the map that was current when it unloaded
 * ({@code owners.dat}; blocks from before this existed belong to pc_pit_flings, the only map that used this world's band);</li>
 * <li>the stores are files in the world folder ({@code portalcraft_chambers/<map>.dat}), written BEFORE a block is removed: a failed write
 * stops all swapping for the session (blocks then stay where they are; nothing is ever deleted without its copy on disk).</li>
 * </ul>
 * The terrain world only (the mirror world keeps one chamber at a time and SkyDig's own clean-up); {@code -Dportalcraft.chamberSlots=false}
 * turns it off.
 */
public final class ChamberSlots {
	private ChamberSlots() {
	}

	/** Minecraft y from which the world is the host's chambers' (HostWorld's band). */
	public static final int BAND_MIN_Y = 320;
	private static final boolean ENABLED = "terrain".equals(System.getProperty("skycraft.world", "mirror")) && !"false".equals(System.getProperty("portalcraft.chamberSlots"));
	/** The owner of band blocks made before bridge #78: pc_pit_flings's chamber was the only one in this world. */
	static final int LEGACY = mapId("pc_pit_flings");
	private static final int CLEAR_FLAGS = Block.UPDATE_CLIENTS | Block.UPDATE_KNOWN_SHAPE | Block.UPDATE_SUPPRESS_DROPS | Block.UPDATE_SKIP_BLOCK_ENTITY_SIDEEFFECTS | Block.UPDATE_SKIP_ON_PLACE;
	private static final int PLACE_FLAGS = Block.UPDATE_CLIENTS | Block.UPDATE_KNOWN_SHAPE | Block.UPDATE_SKIP_ON_PLACE;

	private static int current; // the map whose blocks the loaded chunks' bands hold (0: not known yet this session)
	private static boolean broken; // a store could not be written: no more swapping this session
	private static @org.jspecify.annotations.Nullable Path dir;
	private static final SkyLink.SkyState SKY = new SkyLink.SkyState();
	private static final Map<Integer, Map<Long, CompoundTag>> STORES = new HashMap<>(); // map -> chunk -> {b: blocks, e: entities}
	private static final Set<Integer> DIRTY = new HashSet<>();
	private static final Map<Long, Integer> OWNERS = new HashMap<>(); // chunk -> the map whose blocks it holds (kept for unloaded chunks)
	private static boolean ownersDirty;
	private static final Set<Long> LOADED = new HashSet<>();
	private static final Map<Long, int[]> RECENT = new HashMap<>(); // chunk swapped as it loaded -> {from map, until tick}: its entities load after it
	private static int ticks, swaps, blocksOut, blocksIn, entitiesOut, entitiesIn;
	private static boolean restoring; // (the entities a swap puts back are not another map's)

	/** SkyDig: the other maps' dug cells stay (their revealed blocks are stored with their map here), nothing is cleaned on a map change. */
	public static boolean keepsOtherMaps() {
		return ENABLED && !broken;
	}

	/** The host's map id: FNV-1a of the map name (mc.cpp McLevelInit). */
	static int mapId(String map) {
		int h = 0x811c9dc5;
		for (byte b : map.getBytes(java.nio.charset.StandardCharsets.UTF_8)) {
			h = (h ^ (b & 0xff)) * 16777619;
		}
		return h;
	}

	public static void init() {
		if (!ENABLED) {
			return;
		}
		ServerLifecycleEvents.SERVER_STARTED.register(server -> {
			dir = server.getWorldPath(LevelResource.ROOT).resolve("portalcraft_chambers");
			current = 0;
			broken = false;
			STORES.clear();
			DIRTY.clear();
			OWNERS.clear();
			RECENT.clear();
			loadOwners();
			SkyCraft.LOG.info("Portalcraft chambers: per-map chamber blocks in {} ({} chunks hold a stored map's blocks)", dir, OWNERS.size());
		});
		ServerLifecycleEvents.SERVER_STOPPING.register(server -> {
			ServerLevel level = server.overworld();
			if (level != null) {
				noteLoadedOwners(level);
			}
			flushAll();
		});
		ServerLifecycleEvents.SERVER_STOPPED.register(server -> {
			LOADED.clear();
			current = 0;
		});
		ServerChunkEvents.CHUNK_LOAD.register((level, chunk, generated) -> {
			if (level.dimension() != Level.OVERWORLD) {
				return;
			}
			long key = chunk.getPos().pack();
			LOADED.add(key);
			if (current == 0 || broken) {
				return;
			}
			int from = ownerOf(key, hasBand(chunk));
			if (from != current) {
				RECENT.put(key, new int[] { from, ticks + 400 });
				swap(level, chunk, from, current, true);
			}
		});
		ServerChunkEvents.CHUNK_UNLOAD.register((level, chunk) -> {
			if (level.dimension() != Level.OVERWORLD) {
				return;
			}
			long key = chunk.getPos().pack();
			LOADED.remove(key);
			if (current == 0) {
				return;
			}
			noteOwner(key, chunk, current);
		});
		ServerEntityEvents.ENTITY_LOAD.register((entity, level) -> {
			if (restoring || current == 0 || broken || level.dimension() != Level.OVERWORLD || RECENT.isEmpty() || !movable(entity)) {
				return;
			}
			int[] r = RECENT.get(entity.chunkPosition().pack());
			if (r != null && r[0] != current && ticks < r[1] && stashEntity(level, entity, r[0]) && flush(r[0])) {
				discard(entity);
			}
		});
		ServerTickEvents.END_SERVER_TICK.register(ChamberSlots::tick);
		net.fabricmc.fabric.api.command.v2.CommandRegistrationCallback.EVENT.register((dispatcher, context, environment) -> dispatcher.register(
			net.minecraft.commands.Commands.literal("pcchambers").requires(net.minecraft.commands.Commands.hasPermission(net.minecraft.commands.Commands.LEVEL_GAMEMASTERS)).executes(c -> {
				String st = status();
				SkyCraft.LOG.info("Portalcraft chambers: {}", st);
				c.getSource().sendSuccess(() -> net.minecraft.network.chat.Component.literal("pcchambers: " + st), false);
				return 1;
			})));
		SkyCraft.LOG.info("Portalcraft chambers: per-map chamber blocks above y {} (legacy blocks belong to map {})", BAND_MIN_Y, Integer.toHexString(LEGACY));
	}

	private static void tick(MinecraftServer server) {
		ticks++;
		if (!RECENT.isEmpty() && ticks % 100 == 0) {
			RECENT.values().removeIf(r -> ticks >= r[1]);
		}
		ServerLevel level = server.overworld();
		if (level == null || broken) {
			return;
		}
		if (ticks % 200 == 0 && current != 0) {
			noteLoadedOwners(level);
			flushOwners();
		}
		if (!SkyLink.active() || !SkyLink.readSkyState(SKY)) {
			return;
		}
		int w = SKY.worldId;
		if (w == 0 || w == current) {
			return;
		}
		switchTo(level, w);
	}

	/** The host names map w: every loaded chunk's band goes over to it. */
	private static void switchTo(ServerLevel level, int w) {
		int was = current;
		int chunks = 0, before = blocksOut, beforeIn = blocksIn, eBefore = entitiesOut, eInBefore = entitiesIn;
		// the entities first (their chunk's blocks still stand): each under the map its chunk holds
		List<Entity> ents = new ArrayList<>();
		for (Entity e : level.getAllEntities()) {
			if (movable(e) && LOADED.contains(e.chunkPosition().pack())) {
				ents.add(e);
			}
		}
		List<Entity> stashed = new ArrayList<>();
		for (Entity e : ents) {
			long key = e.chunkPosition().pack();
			int from = was != 0 ? was : ownerOf(key, true);
			if (from != w && stashEntity(level, e, from)) {
				stashed.add(e);
			}
		}
		boolean written = true;
		for (int map : new ArrayList<>(DIRTY)) {
			written &= flush(map);
		}
		if (written) {
			stashed.forEach(ChamberSlots::discard);  // (their copies are on disk)
		}
		for (long key : new ArrayList<>(LOADED)) {
			LevelChunk chunk = level.getChunkSource().getChunkNow(ChunkPos.getX(key), ChunkPos.getZ(key));
			if (chunk == null) {
				continue;
			}
			int from = was != 0 ? was : ownerOf(key, hasBand(chunk));
			if (from == w) {
				continue;
			}
			int moved = blocksOut + blocksIn;
			if (!swap(level, chunk, from, w, true)) {
				break;
			}
			if (blocksOut + blocksIn != moved) {
				chunks++;
			}
		}
		current = w;
		RECENT.clear();
		noteLoadedOwners(level);
		flushAll();
		SkyCraft.LOG.info("Portalcraft chambers: map {} -> {}: {} chunks swapped, {} blocks stored, {} put back, {} entities stored, {} put back{}", Integer.toHexString(was), Integer.toHexString(w), chunks,
			blocksOut - before, blocksIn - beforeIn, entitiesOut - eBefore, entitiesIn - eInBefore, broken ? " (STOPPED: a store could not be written; blocks are left where they are)" : "");
	}

	/** Who holds this chunk's band: its recorded owner, else pc_pit_flings when it has band blocks (made before bridge #78), else the current map. */
	/** Whose blocks this chunk's band holds: its record; else pc_pit_flings's when it has band blocks (made before bridge #78); else nobody's (0: an
	 * empty band, the map it is loaded for gets its stored blocks put in). */
	private static int ownerOf(long key, boolean band) {
		Integer o = OWNERS.get(key);
		if (o != null) {
			return o;
		}
		return band ? LEGACY : 0;
	}

	/** The record a chunk needs: the map whose blocks it holds, while it holds any or that map has blocks stored for it (else an empty band
	 * would get that map's old blocks back when it loads for it again). */
	private static void noteOwner(long key, LevelChunk chunk, int map) {
		setOwner(key, map != 0 && (hasBand(chunk) || store(map).containsKey(key)) ? map : 0);
	}

	private static void setOwner(long key, int map) {
		Integer o = OWNERS.get(key);
		if (map == 0 ? o != null : (o == null || o != map)) {
			if (map == 0) {
				OWNERS.remove(key);
			} else {
				OWNERS.put(key, map);
			}
			ownersDirty = true;
		}
	}

	/** Loaded chunks hold the current map's blocks. */
	private static void noteLoadedOwners(ServerLevel level) {
		if (current == 0) {
			return;
		}
		for (long key : LOADED) {
			LevelChunk chunk = level.getChunkSource().getChunkNow(ChunkPos.getX(key), ChunkPos.getZ(key));
			if (chunk != null) {
				noteOwner(key, chunk, current);
			}
		}
	}

	private static boolean hasBand(LevelChunk chunk) {
		LevelChunkSection[] sections = chunk.getSections();
		for (int i = 0; i < sections.length; i++) {
			if (chunk.getSectionYFromSectionIndex(i) * 16 + 15 >= BAND_MIN_Y && !sections[i].hasOnlyAir()) {
				return true;
			}
		}
		return false;
	}

	/** Entities the band's swap carries: not players, not the host's own (its actors' stand-ins are the host's), not riders (their vehicle carries them), nothing a player rides. */
	private static boolean movable(Entity e) {
		if (e instanceof Player || e.isPassenger() || e.isRemoved() || e.getY() < BAND_MIN_Y || e.getClass().getName().startsWith("dev.skycraft.")) {
			return false;
		}
		for (Entity p : e.getIndirectPassengers()) {
			if (p instanceof Player) {
				return false;
			}
		}
		return true;
	}

	/** An entity (with its riders) into a map's store; the caller writes the store, then discards it. */
	private static boolean stashEntity(ServerLevel level, Entity e, int map) {
		TagValueOutput out = TagValueOutput.createWithContext(ProblemReporter.DISCARDING, level.registryAccess());
		if (!e.save(out)) {
			return false;
		}
		CompoundTag chunkTag = store(map).computeIfAbsent(e.chunkPosition().pack(), k -> new CompoundTag());
		ListTag list = chunkTag.getListOrEmpty("e");
		list.add(out.buildResult());
		chunkTag.put("e", list);
		DIRTY.add(map);
		return true;
	}

	private static void discard(Entity e) {
		e.getSelfAndPassengers().toList().forEach(Entity::discard);
		entitiesOut++;
	}

	/** One chunk's band from map `from` to map `to`. False when a store could not be written (nothing was removed then). */
	private static boolean swap(ServerLevel level, LevelChunk chunk, int from, int to, boolean live) {
		long key = chunk.getPos().pack();
		ListTag out = collect(level, chunk);
		if (out.isEmpty() && !store(to).containsKey(key) && (from == 0 || !store(from).containsKey(key))) {
			noteOwner(key, chunk, to);
			return true;  // (nothing either way: the chunk is not touched)
		}
		Map<Long, CompoundTag> fromStore = store(from);
		CompoundTag prev = fromStore.get(key);
		CompoundTag next = prev != null ? prev.copy() : new CompoundTag();
		next.remove("b");
		if (!out.isEmpty()) {
			next.put("b", out);
		}
		if (next.isEmpty()) {
			fromStore.remove(key);
		} else {
			fromStore.put(key, next);
		}
		if (prev != null || !next.isEmpty()) {
			DIRTY.add(from);
			if (!flush(from)) {
				return false;
			}
		}
		// the copy is on disk: the band is cleared, and the new map's blocks go in
		for (Tag t : out) {
			BlockPos pos = BlockPos.of(((CompoundTag) t).getLongOr("p", 0L));
			chunk.setBlockState(pos, Blocks.AIR.defaultBlockState(), CLEAR_FLAGS);
			if (live) {
				level.getChunkSource().blockChanged(pos);
			}
		}
		blocksOut += out.size();
		CompoundTag in = store(to).get(key);
		if (in != null) {
			var blocks = level.holderLookup(Registries.BLOCK);
			for (Tag t : in.getListOrEmpty("b")) {
				CompoundTag b = (CompoundTag) t;
				BlockPos pos = BlockPos.of(b.getLongOr("p", 0L));
				BlockState state = NbtUtils.readBlockState(blocks, b.getCompoundOrEmpty("s"));
				chunk.setBlockState(pos, state, PLACE_FLAGS);
				var beTag = b.getCompound("be");
				if (beTag.isPresent() && state.hasBlockEntity()) {
					BlockEntity be = BlockEntity.loadStatic(pos, state, beTag.get(), level.registryAccess());
					if (be != null) {
						chunk.setBlockEntity(be);
					}
				}
				if (live) {
					level.getChunkSource().blockChanged(pos);
				}
				blocksIn++;
			}
			ListTag ents = in.getListOrEmpty("e");
			if (!ents.isEmpty()) {
				// (they come back once: the store keeps no copy of entities in the world)
				CompoundTag kept = in.copy();
				kept.remove("e");
				store(to).put(key, kept);
				DIRTY.add(to);
				restoring = true;
				try {
					for (Tag t : ents) {
						Entity e = EntityType.loadEntityRecursive(TagValueInput.create(ProblemReporter.DISCARDING, level.registryAccess(), (CompoundTag) t), level, EntitySpawnReason.LOAD, x -> x);
						if (e != null && level.tryAddFreshEntityWithPassengers(e)) {
							entitiesIn++;
						}
					}
				} finally {
					restoring = false;
				}
			}
		}
		chunk.markUnsaved();
		noteOwner(key, chunk, to);
		swaps++;
		return true;
	}

	/** The band's blocks of a chunk: {p: position, s: state, be: block entity}. */
	private static ListTag collect(ServerLevel level, LevelChunk chunk) {
		ListTag list = new ListTag();
		LevelChunkSection[] sections = chunk.getSections();
		int cx = chunk.getPos().x() * 16, cz = chunk.getPos().z() * 16;
		for (int i = 0; i < sections.length; i++) {
			int sy = chunk.getSectionYFromSectionIndex(i);
			LevelChunkSection section = sections[i];
			if (sy * 16 + 15 < BAND_MIN_Y || section.hasOnlyAir()) {
				continue;
			}
			for (int y = 0; y < 16; y++) {
				int wy = sy * 16 + y;
				if (wy < BAND_MIN_Y) {
					continue;
				}
				for (int z = 0; z < 16; z++) {
					for (int x = 0; x < 16; x++) {
						BlockState state = section.getBlockState(x, y, z);
						if (state.isAir()) {
							continue;
						}
						BlockPos pos = new BlockPos(cx + x, wy, cz + z);
						CompoundTag e = new CompoundTag();
						e.putLong("p", pos.asLong());
						e.put("s", NbtUtils.writeBlockState(state));
						if (state.hasBlockEntity()) {
							BlockEntity be = chunk.getBlockEntity(pos);
							if (be != null) {
								e.put("be", be.saveWithFullMetadata(level.registryAccess()));
							}
						}
						list.add(e);
					}
				}
			}
		}
		return list;
	}

	// ---- files -----------------------------------------------------------------------------------------------------

	private static Map<Long, CompoundTag> store(int map) {
		return STORES.computeIfAbsent(map, ChamberSlots::readStore);
	}

	private static Path storePath(int map) {
		return dir.resolve(String.format("%08x.dat", map));
	}

	private static Map<Long, CompoundTag> readStore(int map) {
		Map<Long, CompoundTag> m = new HashMap<>();
		if (dir == null) {
			return m;
		}
		Path p = storePath(map);
		if (!Files.exists(p)) {
			return m;
		}
		try {
			CompoundTag root = NbtIo.readCompressed(p, NbtAccounter.unlimitedHeap());
			for (Tag t : root.getListOrEmpty("chunks")) {
				CompoundTag c = (CompoundTag) t;
				CompoundTag body = c.copy();
				body.remove("k");
				m.put(c.getLongOr("k", 0L), body);
			}
		} catch (IOException e) {
			// a store that cannot be read must never be overwritten by an empty one
			broken = true;
			SkyCraft.LOG.error("Portalcraft chambers: cannot read {} ({}); per-map chamber blocks are off for this session, the file is left as it is", p, e.toString());
		}
		return m;
	}

	private static boolean flush(int map) {
		if (!DIRTY.contains(map)) {
			return true;
		}
		if (dir == null || broken) {
			broken = true;
			return false;
		}
		CompoundTag root = new CompoundTag();
		root.putInt("map", map);
		ListTag chunks = new ListTag();
		for (Map.Entry<Long, CompoundTag> e : store(map).entrySet()) {
			CompoundTag c = e.getValue().copy();
			c.putLong("k", e.getKey());
			chunks.add(c);
		}
		root.put("chunks", chunks);
		try {
			Files.createDirectories(dir);
			Path p = storePath(map), tmp = dir.resolve(p.getFileName() + ".tmp");
			NbtIo.writeCompressed(root, tmp);
			Files.move(tmp, p, StandardCopyOption.REPLACE_EXISTING, StandardCopyOption.ATOMIC_MOVE);
			DIRTY.remove(map);
			return true;
		} catch (IOException e) {
			broken = true;
			SkyCraft.LOG.error("Portalcraft chambers: cannot write the store of map {} ({}); swapping stops, blocks stay where they are", Integer.toHexString(map), e.toString());
			return false;
		}
	}

	private static void flushAll() {
		for (int map : new ArrayList<>(DIRTY)) {
			flush(map);
		}
		flushOwners();
	}

	private static void loadOwners() {
		Path p = dir.resolve("owners.dat");
		if (!Files.exists(p)) {
			return;
		}
		try {
			CompoundTag root = NbtIo.readCompressed(p, NbtAccounter.unlimitedHeap());
			for (Tag t : root.getListOrEmpty("owners")) {
				CompoundTag c = (CompoundTag) t;
				OWNERS.put(c.getLongOr("k", 0L), c.getIntOr("w", 0));
			}
		} catch (IOException e) {
			broken = true;
			SkyCraft.LOG.error("Portalcraft chambers: cannot read {} ({}); per-map chamber blocks are off for this session", p, e.toString());
		}
	}

	private static void flushOwners() {
		if (!ownersDirty || dir == null || broken) {
			return;
		}
		CompoundTag root = new CompoundTag();
		ListTag list = new ListTag();
		for (Map.Entry<Long, Integer> e : OWNERS.entrySet()) {
			CompoundTag c = new CompoundTag();
			c.putLong("k", e.getKey());
			c.putInt("w", e.getValue());
			list.add(c);
		}
		root.put("owners", list);
		try {
			Files.createDirectories(dir);
			Path p = dir.resolve("owners.dat"), tmp = dir.resolve("owners.dat.tmp");
			NbtIo.writeCompressed(root, tmp);
			Files.move(tmp, p, StandardCopyOption.REPLACE_EXISTING, StandardCopyOption.ATOMIC_MOVE);
			ownersDirty = false;
		} catch (IOException e) {
			SkyCraft.LOG.error("Portalcraft chambers: cannot write owners.dat ({})", e.toString());
		}
	}

	/** /pcchambers (op): what is stored, for tests. */
	public static String status() {
		StringBuilder b = new StringBuilder();
		b.append(String.format("map %08x, %d chunks swapped, blocks %d out / %d in, entities %d out / %d in, %d owner records%s", current, swaps, blocksOut, blocksIn, entitiesOut, entitiesIn, OWNERS.size(), broken ? ", STOPPED" : ""));
		for (Map.Entry<Integer, Map<Long, CompoundTag>> e : STORES.entrySet()) {
			int n = 0;
			for (CompoundTag c : e.getValue().values()) {
				n += c.getListOrEmpty("b").size();
			}
			b.append(String.format("; %08x: %d chunks, %d blocks", e.getKey(), e.getValue().size(), n));
		}
		return b.toString();
	}
}
