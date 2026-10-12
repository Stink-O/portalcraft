package dev.skycraft.client;

import dev.skycraft.client.render.WorldExporter;
import dev.skycraft.world.DigHost;
import dev.skycraft.world.DigMath;
import dev.skycraft.world.ChamberBlocks;
import dev.skycraft.world.SkyClip;
import dev.skycraft.world.SkyCollision;
import dev.skycraft.world.SkyDig;
import dev.skycraft.world.SkyMovers;
import it.unimi.dsi.fastutil.longs.Long2ObjectOpenHashMap;
import java.util.List;
import net.fabricmc.fabric.api.client.networking.v1.ClientPlayNetworking;
import net.minecraft.client.Minecraft;
import net.minecraft.client.multiplayer.ClientLevel;
import net.minecraft.client.particle.TerrainParticle;
import net.minecraft.client.resources.sounds.SimpleSoundInstance;
import net.minecraft.client.resources.sounds.SoundInstance;
import net.minecraft.core.BlockPos;
import net.minecraft.sounds.SoundSource;
import net.minecraft.world.level.ChunkPos;
import net.minecraft.world.level.GameType;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.chunk.LevelChunk;
import net.minecraft.world.level.chunk.status.ChunkStatus;
import org.jspecify.annotations.Nullable;

/**
 * The digging player's side of {@link SkyDig} (Portalcraft, bridge #33). Ported from SkyCraft's SkyDigClient (MIT,
 * chasmlol/SkyCraft; see LICENSE-SkyCraft): hitting the host's chamber surface under the crosshair mines it, as long as
 * mining a block of that material would take with the held tool, then asks the server to open the cell holding the hit
 * voxel; the dug cells of loaded chunks go to the host (WorldExporter sends REN_DUG per section, and the wall faces
 * that close the hole are part of the section mesh). Everything is inert unless the host sets HP_DIG with a dig world.
 */
public final class SkyDigClient {
	private SkyDigClient() {
	}

	// Mining a host surface: which cell, how far along (0..1), on which tick last, and the host material when it started.
	private static @Nullable BlockPos mining;
	private static float progress;
	private static int miningTick;
	private static int miningMaterial;
	private static int hits;
	private static int ticks;
	private static int cooldownUntil;
	private static final Long2ObjectOpenHashMap<SkyDig.DugColumn> SEEN = new Long2ObjectOpenHashMap<>(); // client chunk -> the column last handled
	private static final SkyDig.DugColumn NONE = SkyDig.DugColumn.EMPTY;
	private static int seenWorld = Integer.MIN_VALUE;
	private static boolean seenEnabled;
	private static int pollTicks;

	/** Called from the client mod initialiser. */
	public static void init() {
		SkyCollision.setChangeRecorder(() -> SkyDig.anyDug() && DigHost.enabled());
		net.fabricmc.fabric.api.client.event.lifecycle.v1.ClientTickEvents.END_CLIENT_TICK.register(SkyDigClient::tick);
		net.fabricmc.fabric.api.client.networking.v1.ClientPlayConnectionEvents.DISCONNECT.register((handler, minecraft) -> {
			SEEN.clear();
			if (!minecraft.hasSingleplayerServer()) {
				SkyDig.clearMirror();
			}
		});
	}

	/**
	 * The player attacked (clicked or held) a host surface under the crosshair: mining it, as long as mining the block it is
	 * made of would take. Returns true if it is being mined.
	 */
	public static boolean attack(Minecraft minecraft) {
		if (minecraft.level == null || minecraft.player == null || minecraft.gameMode == null) {
			return false;
		}
		if (!SkyDig.destruction || !DigHost.enabled()) {
			if (minecraft.hitResult instanceof SkyClip.SkyrimHitResult) {
				note("off", "attack on the host's surface ignored: " + (!SkyDig.destruction ? "/pcdig destruction is off" : "the host does not dig (HP_DIG clear, or no dig world yet)"));
			}
			return false;
		}
		if (minecraft.hitResult instanceof SkyClip.MoverHitResult moverHit) {
			return attackMover(minecraft, moverHit); // (bridge #43) a door, a panel, a lift...: a hole in that one mover
		}
		if (!(minecraft.hitResult instanceof SkyClip.SkyrimHitResult result)) {
			// (bridge #38) every other click that is not on the host's surface says why not, once in a while: a MISS or a block with the eye inside the host's solid
			// (dig8: the test stood inside the wall brush, so the cell holding the eye was never a target and every ray started inside it)
			var eye = minecraft.player.getEyePosition();
			BlockPos cell = BlockPos.containing(eye);
			if (eyeInsideHost(cell, eye)) {
				note("eye", "attack: the eye (" + String.format("%.2f %.2f %.2f", eye.x, eye.y, eye.z) + ") is INSIDE the host's solid in cell " + cell.toShortString()
					+ ": that cell is never a target from in there (the pick looks past it); walk out of the wall first. The click was on " + minecraft.hitResult.getType());
			} else {
				note("vanilla", "attack: not on the host's surface (" + minecraft.hitResult.getType() + "): vanilla handles it");
			}
			return false;
		}
		GameType mode = minecraft.gameMode.getPlayerMode();
		if (mode == GameType.ADVENTURE || mode == GameType.SPECTATOR) {
			note("mode", "attack on the host's surface ignored: game mode " + mode);
			return false;
		}
		BlockPos pos = result.surface; // the cell holding the hit voxel
		int world = DigHost.world();
		if (!SkyCollision.rawHasVisible(pos) || SkyDig.isDug(world, pos)) {
			note("nogeo", "attack on host cell " + pos.toShortString() + ": " + (SkyDig.isDug(world, pos) ? "already dug" : "nothing visible there (the export changed under the click, or only invisible collision)"));
			return false;
		}
		if (SkyCollision.rawLimit(pos)) {
			// (bridge #38) the one place digging stops: the edge of what Portal 2 can hold (it kills what leaves +-16384 u)
			refuse(minecraft, pos, "this is the bottom of the world (the edge of what Portal 2 can hold)");
			return false;
		}
		BlockState here = minecraft.level.getBlockState(pos);
		if (!here.isAir() && !here.canBeReplaced()) {
			note("block", "attack on host cell " + pos.toShortString() + ": a real block (" + here.getBlock().getName().getString() + ") is in it; vanilla mines that");
			return false;
		}
		if (ticks < cooldownUntil) {
			return true;
		}
		// Once a tick (a click and a held button can both land in the same one).
		if (pos.equals(mining) && miningTick == ticks) {
			return true;
		}
		if (!pos.equals(mining) || ticks - miningTick > 2) {
			mining = pos;
			progress = 0.0F;
			hits = 0;
			miningMaterial = DigMath.materialOf(DigHost.material()); // what the host says the surface is, when mining starts
			BlockState st = ChamberBlocks.stateFor(miningMaterial);
			note("start", "mining host cell " + pos.toShortString() + " (hit " + String.format("%.2f %.2f %.2f", result.getLocation().x, result.getLocation().y, result.getLocation().z) + ", face " + result.getDirection()
				+ ") as " + DigMath.blockName(miningMaterial) + ": " + String.format("%.3f", minecraft.player.getAbilities().instabuild ? 1.0F : st.getDestroyProgress(minecraft.player, minecraft.level, pos))
				+ " per tick with " + minecraft.player.getMainHandItem().getHoverName().getString() + (st.requiresCorrectToolForDrops() && !minecraft.player.hasCorrectToolForDrops(st) ? " (wrong tool: no drops, and slow)" : ""));
		}
		miningTick = ticks;
		BlockState state = ChamberBlocks.stateFor(miningMaterial);
		boolean creative = minecraft.player.getAbilities().instabuild;
		progress += creative ? 1.0F : state.getDestroyProgress(minecraft.player, minecraft.level, pos);
		if (hits++ % 4 == 0) {
			var sound = state.getSoundType();
			minecraft.getSoundManager().play(new SimpleSoundInstance(
				sound.getHitSound(), SoundSource.BLOCKS, (sound.getVolume() + 1.0F) / 8.0F, sound.getPitch() * 0.5F, SoundInstance.createUnseededRandom(), pos
			));
			var loc = result.getLocation();
			minecraft.particleEngine.add(new TerrainParticle(minecraft.level, loc.x + result.nx * 0.05, loc.y + result.ny * 0.05, loc.z + result.nz * 0.05, 0.0, 0.0, 0.0, state, pos)
				.setPower(0.2F).scale(0.6F));
		}
		if (progress >= 1.0F) {
			note("send", "mined host cell " + pos.toShortString() + " in " + hits + " ticks: asking the server to open it");
			ClientPlayNetworking.send(new SkyDig.DigOpen(world, pos, miningMaterial));
			mining = null;
			progress = 0.0F;
			if (creative) {
				cooldownUntil = ticks + 5; // like vanilla's block-break delay in creative
			}
		}
		return true;
	}

	// (bridge #43) Mining a mover: which mover and cell, how far along, on which tick last, and the cells just sent (the old shape is still in use until the host's answer arrives).
	private static int movingId;
	private static int mcx;
	private static int mcy;
	private static int mcz;
	private static float moverProgress;
	private static int moverTick = -1000;
	private static int moverHits;
	private static final java.util.HashMap<String, Integer> SENT = new java.util.HashMap<>();

	/**
	 * The player attacked a host mover under the crosshair. One rule for every mover: the cell of the mover's own dig lattice (a 1-block cube in its frame) holding
	 * the hit voxel is mined as a block of the surface's material would be, then the host is told (REN_MOVER_DIG); it keeps the set of that mover's holes, answers
	 * with the mover's shape minus them, and the hole goes where the mover goes.
	 */
	private static boolean attackMover(Minecraft minecraft, SkyClip.MoverHitResult result) {
		GameType mode = minecraft.gameMode.getPlayerMode();
		if (mode == GameType.ADVENTURE || mode == GameType.SPECTATOR) {
			note("mode", "attack on a host mover ignored: game mode " + mode);
			return false;
		}
		SkyMovers.Hit h = result.mover;
		if (!h.minable()) {
			note("mover-nopick", "attack on host mover #" + h.moverId() + ": nothing to mine in it (a physics prop / NPC / held object, no dig lattice for its shape, or invisible geometry)");
			return false;
		}
		BlockPos pos = result.getBlockPos();
		String key = h.moverId() + ":" + h.cx() + "," + h.cy() + "," + h.cz();
		Integer sentAt = SENT.get(key);
		if (sentAt != null && ticks - sentAt < 20) {
			return true; // just mined: the host's new shape for the mover is on its way
		}
		if (ticks < cooldownUntil) {
			return true;
		}
		if (h.moverId() == movingId && h.cx() == mcx && h.cy() == mcy && h.cz() == mcz && moverTick == ticks) {
			return true; // once a tick
		}
		mining = null;
		progress = 0.0F;
		if (h.moverId() != movingId || h.cx() != mcx || h.cy() != mcy || h.cz() != mcz || ticks - moverTick > 2) {
			movingId = h.moverId();
			mcx = h.cx();
			mcy = h.cy();
			mcz = h.cz();
			moverProgress = 0.0F;
			moverHits = 0;
			miningMaterial = DigMath.materialOf(DigHost.material());
			BlockState st = ChamberBlocks.stateFor(miningMaterial);
			note("start", "mining host mover #" + h.moverId() + " cell " + h.cx() + " " + h.cy() + " " + h.cz() + " (hit " + String.format("%.2f %.2f %.2f", h.x(), h.y(), h.z()) + ") as "
				+ DigMath.blockName(miningMaterial) + ": " + String.format("%.3f", minecraft.player.getAbilities().instabuild ? 1.0F : st.getDestroyProgress(minecraft.player, minecraft.level, pos)) + " per tick");
		}
		moverTick = ticks;
		BlockState state = ChamberBlocks.stateFor(miningMaterial);
		boolean creative = minecraft.player.getAbilities().instabuild;
		moverProgress += creative ? 1.0F : state.getDestroyProgress(minecraft.player, minecraft.level, pos);
		if (moverHits++ % 4 == 0) {
			var sound = state.getSoundType();
			minecraft.getSoundManager().play(new SimpleSoundInstance(
				sound.getHitSound(), SoundSource.BLOCKS, (sound.getVolume() + 1.0F) / 8.0F, sound.getPitch() * 0.5F, SoundInstance.createUnseededRandom(), pos
			));
			var loc = result.getLocation();
			minecraft.particleEngine.add(new TerrainParticle(minecraft.level, loc.x + result.nx * 0.05, loc.y + result.ny * 0.05, loc.z + result.nz * 0.05, 0.0, 0.0, 0.0, state, pos)
				.setPower(0.2F).scale(0.6F));
		}
		if (moverProgress >= 1.0F) {
			note("send", "mined host mover #" + h.moverId() + " cell " + h.cx() + " " + h.cy() + " " + h.cz() + " in " + moverHits + " ticks: telling the host");
			sendMoverStyles(minecraft); // (a host that started after the last periodic send has the style before the cell arrives)
			if (SkyMovers.sendDig(h.moverId(), h.cx(), h.cy(), h.cz(), DigHost.world(), miningMaterial)) {
				SENT.put(key, ticks);
				if (SENT.size() > 200) {
					SENT.clear();
				}
			}
			movingId = 0;
			moverProgress = 0.0F;
			if (creative) {
				cooldownUntil = ticks + 5;
			}
		}
		return true;
	}

	/** The eye point lies in a solid sub-voxel of the host's export (not in a dug cell). */
	private static boolean eyeInsideHost(BlockPos cell, net.minecraft.world.phys.Vec3 eye) {
		if (SkyDig.isDug(DigHost.world(), cell)) {
			return false;
		}
		long[] bits = SkyCollision.rawBits(cell);
		if (bits == null) {
			return false;
		}
		int sx = Math.min(7, Math.max(0, (int) Math.floor((eye.x - cell.getX()) * 8)));
		int sy = Math.min(7, Math.max(0, (int) Math.floor((eye.y - cell.getY()) * 8)));
		int sz = Math.min(7, Math.max(0, (int) Math.floor((eye.z - cell.getZ()) * 8)));
		return ((bits[sy] >>> (sz * 8 + sx)) & 1L) != 0;
	}

	private static final java.util.HashMap<String, Integer> NOTED = new java.util.HashMap<>();
	private static int notedLines;

	/** (bridge #38) A log line for every way an attack on the host's surface goes, at most one per kind per 2 s (a held button calls this every tick). */
	private static void note(String kind, String text) {
		Integer last = NOTED.get(kind + (kind.equals("start") || kind.equals("send") ? "" : text));
		if ((last != null && ticks - last < 40 && !kind.equals("send")) || notedLines > 2000) {
			return;
		}
		NOTED.put(kind + (kind.equals("start") || kind.equals("send") ? "" : text), ticks);
		if (NOTED.size() > 500) {
			NOTED.clear();
		}
		++notedLines;
		dev.skycraft.SkyCraft.LOG.info("Portalcraft dig (client): {}", text);
	}

	private static final java.util.HashSet<Long> REFUSED = new java.util.HashSet<>();
	private static int refusedMessageAt = -1000;

	/** Bridge #35: a refused dig is said so (dig2: clicking the hall floor's second cell did nothing, silently): a log line per cell and an action-bar line. */
	private static void refuse(Minecraft minecraft, BlockPos pos, String why) {
		if (REFUSED.size() < 200 && REFUSED.add(pos.asLong())) {
			dev.skycraft.SkyCraft.LOG.info("Portalcraft dig: refused cell {} {} {}: {}", pos.getX(), pos.getY(), pos.getZ(), why);
		}
		if (ticks - refusedMessageAt > 40) {
			refusedMessageAt = ticks;
			minecraft.player.sendOverlayMessage(net.minecraft.network.chat.Component.literal("Can't dig here: " + why));
		}
	}

	/**
	 * Any block change on this client: a block of a dug cell that went (a player broke it, an explosion, a piston) has the cells around it
	 * revealed. Ported from SkyCraft's SkyDigClient.blockChanged / ClientLevelDigMixin; here the integrated server does the reveal
	 * (it holds the same host field), where SkyCraft's client sends a packet.
	 */
	public static void blockChanged(ClientLevel level, BlockPos pos, BlockState before, BlockState after) {
		if (before.isAir() || !(after.isAir() || after.canBeReplaced())) {
			return;
		}
		Minecraft minecraft = Minecraft.getInstance();
		if (minecraft.player == null || !SkyDig.destruction || !DigHost.enabled() || minecraft.player.blockPosition().distSqr(pos) > 48 * 48) {
			return;
		}
		if (!SkyDig.isDug(DigHost.world(), pos)) {
			return;
		}
		var server = minecraft.getSingleplayerServer();
		if (server == null) {
			return;
		}
		BlockPos at = pos.immutable();
		server.execute(() -> {
			var serverLevel = server.overworld();
			if (serverLevel != null) {
				SkyDig.revealAfterBreak(serverLevel, at, before);
			}
		});
	}

	public static void tick(Minecraft minecraft) {
		ticks++;
		ClientLevel level = minecraft.level;
		if (level == null || minecraft.player == null) {
			SEEN.clear();
			return;
		}
		boolean enabled = DigHost.enabled();
		if (!enabled && !seenEnabled && SEEN.isEmpty()) {
			return; // the host does not dig: nothing to do
		}
		if (++pollTicks % 5 == 0 || enabled != seenEnabled) {
			pollDug(minecraft, level);
		}
		if (enabled) {
			changedRegions();
			if (ticks % 100 == 7) {
				sendMoverStyles(minecraft); // (bridge #50) the host draws the walls of holes mined in movers with these blocks' faces; it may have started after us
			}
		}
	}

	/**
	 * (bridge #50) The atlas rects of the six chamber materials' blocks (side, top, bottom) go to the host (a style message of REN_MOVER_DIG): the walls of a hole mined in a
	 * mover are drawn there, in the mover's frame, as faces of the block the cell was mined as. Sent every 5 s while the host digs (the host may start after the mod, and the
	 * atlas may be rebuilt by a resource reload); six small messages.
	 */
	private static void sendMoverStyles(Minecraft minecraft) {
		for (int kind = DigMath.CH_PANEL_WHITE; kind <= DigMath.CH_CONCRETE; kind++) {
			float[] rects = dev.skycraft.client.render.WorldExporter.cubeFaces(minecraft, ChamberBlocks.stateFor(kind));
			if (rects != null) {
				SkyMovers.sendStyle(kind, rects);
			}
		}
	}

	/** Dug cells of this section in the current dig world, or null (also when the host does not dig). */
	public static long @Nullable [] dugBits(LevelChunk chunk, int sectionY) {
		if (!DigHost.enabled()) {
			return null;
		}
		SkyDig.DugColumn column = chunk.getAttached(SkyDig.DUG);
		return column == null ? null : column.bits(DigHost.world(), sectionY);
	}

	/** The host needs every dug cell again (a new link, or it cleared them). */
	public static void resendAll() {
		SEEN.clear();
		seenWorld = Integer.MIN_VALUE;
	}

	// Chunks whose dug cells changed (or the dig world / HP_DIG changed): their sections go to the host again, and so do the
	// sections next to them (a dug cell opens a wall in the section next door).
	private static void pollDug(Minecraft minecraft, ClientLevel level) {
		int world = DigHost.world();
		boolean enabled = DigHost.enabled();
		boolean modeChanged = world != seenWorld || enabled != seenEnabled;
		seenWorld = world;
		seenEnabled = enabled;
		boolean mirrorHere = !minecraft.hasSingleplayerServer(); // (the integrated server feeds the mirror itself, fresher than the synced copy)
		int radius = minecraft.options.getEffectiveRenderDistance() + 1;
		int pcx = minecraft.player.getBlockX() >> 4, pcz = minecraft.player.getBlockZ() >> 4;
		for (int cx = pcx - radius; cx <= pcx + radius; cx++) {
			for (int cz = pcz - radius; cz <= pcz + radius; cz++) {
				long key = ChunkPos.pack(cx, cz);
				LevelChunk chunk = level.getChunkSource().getChunk(cx, cz, ChunkStatus.FULL, false);
				if (chunk == null) {
					SEEN.remove(key);
					continue;
				}
				SkyDig.DugColumn column = chunk.getAttached(SkyDig.DUG);
				if (column == null) {
					column = NONE;
				}
				SkyDig.DugColumn previous = SEEN.get(key);
				if (previous == column && !modeChanged) {
					continue;
				}
				if (previous == null && column.isEmpty() && !modeChanged) {
					SEEN.put(key, column);
					continue;
				}
				SEEN.put(key, column);
				if (mirrorHere) {
					SkyDig.mirror(key, column);
				}
				for (SkyDig.DugColumn c : new SkyDig.DugColumn[] {column, previous == null ? NONE : previous}) {
					for (SkyDig.DugSection s : c.sections()) {
						if (modeChanged || s.world() == world) {
							markWithNeighbours(cx, s.sectionY(), cz);
						}
					}
				}
			}
		}
		if (SEEN.size() > 8192) {
			SEEN.clear();
		}
	}

	private static void markWithNeighbours(int cx, int sy, int cz) {
		WorldExporter.markDirtyNow(cx, sy, cz);
		WorldExporter.markDirty(cx - 1, sy, cz);
		WorldExporter.markDirty(cx + 1, sy, cz);
		WorldExporter.markDirty(cx, sy - 1, cz);
		WorldExporter.markDirty(cx, sy + 1, cz);
		WorldExporter.markDirty(cx, sy, cz - 1);
		WorldExporter.markDirty(cx, sy, cz + 1);
	}

	// The host's voxel field for a cell next to a dug one arrived (or changed): the walls (in the dug cell's section) are rebuilt.
	private static void changedRegions() {
		int world = DigHost.world();
		boolean complete = SkyCollision.takeChangedRegions(box -> {
			int cx0 = (box[0] - 1) >> 4, cx1 = (box[3] + 1) >> 4, cz0 = (box[2] - 1) >> 4, cz1 = (box[5] + 1) >> 4;
			int sy0 = (box[1] - 1) >> 4, sy1 = (box[4] + 1) >> 4;
			if ((long) (cx1 - cx0 + 1) * (cz1 - cz0 + 1) > 1024) {
				for (long key : SkyDig.mirroredChunkKeys()) {
					int cx = ChunkPos.getX(key), cz = ChunkPos.getZ(key);
					if (cx >= cx0 && cx <= cx1 && cz >= cz0 && cz <= cz1) {
						markSections(key, cx, cz, world, sy0, sy1);
					}
				}
				return;
			}
			for (int cx = cx0; cx <= cx1; cx++) {
				for (int cz = cz0; cz <= cz1; cz++) {
					markSections(ChunkPos.pack(cx, cz), cx, cz, world, sy0, sy1);
				}
			}
		});
		if (!complete) {
			for (long key : SkyDig.mirroredChunkKeys()) {
				markSections(key, ChunkPos.getX(key), ChunkPos.getZ(key), world, Integer.MIN_VALUE, Integer.MAX_VALUE);
			}
		}
	}

	private static void markSections(long chunkKey, int cx, int cz, int world, int sy0, int sy1) {
		SkyDig.DugColumn column = SkyDig.mirrored(chunkKey);
		if (column == null) {
			return;
		}
		for (SkyDig.DugSection s : column.sections()) {
			if (s.world() == world && s.sectionY() >= sy0 && s.sectionY() <= sy1) {
				WorldExporter.markDirty(cx, s.sectionY(), cz);
			}
		}
	}

	/** For tests and logs: the mining state. */
	public static List<String> debug() {
		return List.of("mining " + mining + " progress " + progress + " material " + miningMaterial);
	}

	/** (bridge #53) the sizes of this class's static caches and queues, for the periodic growth line in the log. */
	public static String growth() {
		return "dig client seen " + SEEN.size() + ", sent " + SENT.size() + ", noted " + NOTED.size() + ", refused " + REFUSED.size();
	}
}
