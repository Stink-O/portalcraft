package dev.skycraft.world;

import com.mojang.brigadier.arguments.IntegerArgumentType;
import com.mojang.brigadier.arguments.BoolArgumentType;
import com.mojang.brigadier.context.CommandContext;
import com.mojang.serialization.Codec;
import com.mojang.serialization.codecs.RecordCodecBuilder;
import dev.skycraft.SkyCraft;
import io.netty.buffer.ByteBuf;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.stream.LongStream;
import net.fabricmc.fabric.api.attachment.v1.AttachmentRegistry;
import net.fabricmc.fabric.api.attachment.v1.AttachmentSyncPredicate;
import net.fabricmc.fabric.api.attachment.v1.AttachmentType;
import net.fabricmc.fabric.api.command.v2.CommandRegistrationCallback;
import net.fabricmc.fabric.api.event.lifecycle.v1.ServerChunkEvents;
import net.fabricmc.fabric.api.event.lifecycle.v1.ServerLifecycleEvents;
import net.fabricmc.fabric.api.event.player.PlayerBlockBreakEvents;
import net.fabricmc.fabric.api.networking.v1.PayloadTypeRegistry;
import net.fabricmc.fabric.api.networking.v1.ServerPlayNetworking;
import net.minecraft.commands.CommandSourceStack;
import net.minecraft.commands.Commands;
import net.minecraft.commands.arguments.coordinates.BlockPosArgument;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Direction;
import net.minecraft.network.RegistryFriendlyByteBuf;
import net.minecraft.network.chat.Component;
import net.minecraft.network.codec.ByteBufCodecs;
import net.minecraft.network.codec.StreamCodec;
import net.minecraft.network.protocol.common.custom.CustomPacketPayload;
import net.minecraft.resources.Identifier;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.item.ItemStack;
import net.minecraft.world.level.ChunkPos;
import net.minecraft.world.level.GameType;
import net.minecraft.world.level.Level;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.block.LevelEvent;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.chunk.LevelChunk;
import org.jspecify.annotations.Nullable;

/**
 * Digging into Portal 2's chambers (Portalcraft, bridge #33, reworked in #38). Ported from SkyCraft's SkyDig (MIT, chasmlol/SkyCraft;
 * see LICENSE-SkyCraft): the per-chunk set of dug cells kept as a persistent, synced attachment per host world, open (mining a host
 * surface takes its cell out, with the drops) and reveal (the cells around it that lie inside the host's geometry become real blocks),
 * and the dug-cell filter that removes the host's voxels from Minecraft's collision. Differences: the host's geometry is the voxel field
 * (SkyCollision), so the server classifies the cells itself (the integrated server holds the same field); the blocks are the chamber
 * family (ChamberBlocks) and vanilla stone; everything is gated on the host flag HP_DIG (DigHost): with it clear nothing here does anything.
 *
 * <ul>
 * <li><b>open</b>: a cell with host content (and not at the dig limit) is marked dug, no block placed; the drops of the opened
 * material's block, the break particles and sound.</li>
 * <li><b>reveal</b> (SkyCraft's classify, {@link DigMath#classify}): the face neighbours of an opened cell (or of a broken revealed
 * block) with 25 of 27 sample points inside the host's geometry become blocks, chosen by how deep behind the surface they are: the
 * chamber's own block family for the first layers, concrete, then stone with ores (SkyCraft's stoneOrOre), bedrock at the dig limit.
 * The solid void Portal 2 has behind its walls and floors is in the field as solid: it is deep rock, so a tunnel can leave the map and
 * is always closed by Minecraft blocks. Cells less than 25/27 inside stay host geometry (the client closes the hole with wall faces, see
 * DigWalls).</li>
 * </ul>
 */
public final class SkyDig {
	private SkyDig() {
	}

	/** Whether Minecraft digs into the host at all (default on). {@code /pcdig destruction false} turns it off. */
	public static volatile boolean destruction = true;

	/** One section's dug cells in one host world: bit x + 16z + 256y (DigMath.bitIndex). */
	public record DugSection(int world, int sectionY, long[] bits) {
		static final Codec<DugSection> CODEC = RecordCodecBuilder.create(i -> i.group(
			Codec.INT.fieldOf("world").forGetter(DugSection::world),
			Codec.INT.fieldOf("y").forGetter(DugSection::sectionY),
			Codec.LONG_STREAM.xmap(LongStream::toArray, Arrays::stream).fieldOf("bits").forGetter(DugSection::bits)
		).apply(i, DugSection::new));
	}

	/** A chunk's dug cells. Immutable: changes make a new one (so the attachment syncs). */
	public record DugColumn(List<DugSection> sections) {
		public static final DugColumn EMPTY = new DugColumn(List.of());
		static final Codec<DugColumn> CODEC = DugSection.CODEC.listOf().xmap(DugColumn::new, DugColumn::sections);
		static final StreamCodec<ByteBuf, DugColumn> STREAM_CODEC = new StreamCodec<>() {
			@Override
			public DugColumn decode(ByteBuf buf) {
				int n = ByteBufCodecs.VAR_INT.decode(buf);
				List<DugSection> sections = new ArrayList<>(n);
				for (int i = 0; i < n; i++) {
					int world = buf.readInt();
					int y = ByteBufCodecs.VAR_INT.decode(buf);
					long[] bits = new long[64];
					for (int k = 0; k < 64; k++) {
						bits[k] = buf.readLong();
					}
					sections.add(new DugSection(world, y, bits));
				}
				return new DugColumn(List.copyOf(sections));
			}

			@Override
			public void encode(ByteBuf buf, DugColumn column) {
				ByteBufCodecs.VAR_INT.encode(buf, column.sections.size());
				for (DugSection s : column.sections) {
					buf.writeInt(s.world);
					ByteBufCodecs.VAR_INT.encode(buf, s.sectionY);
					for (int k = 0; k < 64; k++) {
						buf.writeLong(s.bits.length == 64 ? s.bits[k] : 0L);
					}
				}
			}
		};

		public boolean isEmpty() {
			return this.sections.isEmpty();
		}

		public long @Nullable [] bits(int world, int sectionY) {
			for (DugSection s : this.sections) {
				if (s.world == world && s.sectionY == sectionY && s.bits.length == 64) {
					return s.bits;
				}
			}
			return null;
		}

		public boolean isDug(int world, int x, int y, int z) {
			long[] bits = bits(world, y >> 4);
			return bits != null && DigMath.testBit(bits, DigMath.bitIndex(x, y, z));
		}

		/** True if any section of this world has a dug cell. */
		public boolean hasWorld(int world) {
			for (DugSection s : this.sections) {
				if (s.world == world) {
					return true;
				}
			}
			return false;
		}

		/** True if any section belongs to a world the predicate selects. */
		public boolean hasWorldWhere(java.util.function.IntPredicate worlds) {
			for (DugSection s : this.sections) {
				if (worlds.test(s.world)) {
					return true;
				}
			}
			return false;
		}

		DugColumn with(int world, int x, int y, int z) {
			int sy = y >> 4;
			int bit = DigMath.bitIndex(x, y, z);
			List<DugSection> out = new ArrayList<>(this.sections.size() + 1);
			boolean found = false;
			for (DugSection s : this.sections) {
				if (s.world == world && s.sectionY == sy && s.bits.length == 64) {
					long[] bits = s.bits.clone();
					DigMath.setBit(bits, bit);
					out.add(new DugSection(world, sy, bits));
					found = true;
				} else {
					out.add(s);
				}
			}
			if (!found) {
				long[] bits = new long[64];
				DigMath.setBit(bits, bit);
				out.add(new DugSection(world, sy, bits));
			}
			return new DugColumn(List.copyOf(out));
		}

		DugColumn withoutWorld(int world) {
			return withoutWorlds(w -> w == world);
		}

		DugColumn withoutWorlds(java.util.function.IntPredicate worlds) {
			List<DugSection> out = new ArrayList<>(this.sections.size());
			for (DugSection s : this.sections) {
				if (!worlds.test(s.world)) {
					out.add(s);
				}
			}
			return out.isEmpty() ? EMPTY : new DugColumn(List.copyOf(out));
		}
	}

	public static final AttachmentType<DugColumn> DUG = AttachmentRegistry.<DugColumn>builder()
		.persistent(DugColumn.CODEC)
		.syncWith(DugColumn.STREAM_CODEC, AttachmentSyncPredicate.all())
		.buildAndRegister(Identifier.fromNamespaceAndPath(SkyCraft.MOD_ID, "dug"));

	// ---- packets (client -> server) ----------------------------------------------------------------------

	/** The player mined the host's surface in this cell (the cell holding the hit voxel); material = CH_*. */
	public record DigOpen(int world, BlockPos pos, int material) implements CustomPacketPayload {
		public static final Type<DigOpen> TYPE = new Type<>(Identifier.fromNamespaceAndPath(SkyCraft.MOD_ID, "dig_open"));
		public static final StreamCodec<RegistryFriendlyByteBuf, DigOpen> CODEC = StreamCodec.composite(
			ByteBufCodecs.INT, DigOpen::world,
			BlockPos.STREAM_CODEC, DigOpen::pos,
			ByteBufCodecs.VAR_INT, DigOpen::material,
			DigOpen::new
		);

		@Override
		public Type<? extends CustomPacketPayload> type() {
			return TYPE;
		}
	}

	// ---- the dug-cell mirror: what collision queries on any thread ask --------------------------------------

	// Chunk -> its dug cells (all worlds). Fed by the server (every change, chunk load) and the client (synced
	// attachments, SkyDigClient). Same JVM for the integrated server and the client: one overlay for both, so the host's
	// voxels are gone in client and server collision at the same moment.
	private static final ConcurrentHashMap<Long, DugColumn> COLUMNS = new ConcurrentHashMap<>();

	public static void mirror(long chunkKey, @Nullable DugColumn column) {
		if (column == null || column.isEmpty()) {
			COLUMNS.remove(chunkKey);
		} else {
			COLUMNS.put(chunkKey, column);
		}
	}

	public static @Nullable DugColumn mirrored(long chunkKey) {
		return COLUMNS.get(chunkKey);
	}

	public static long[] mirroredChunkKeys() {
		return COLUMNS.keySet().stream().mapToLong(Long::longValue).toArray();
	}

	public static boolean anyDug() {
		return !COLUMNS.isEmpty();
	}

	public static void clearMirror() {
		COLUMNS.clear();
	}

	/** Is this block position dug in the current dig world? (SkyCollision's filter; any thread; false when the host does not dig.) */
	public static boolean isDugKey(long key) {
		if (COLUMNS.isEmpty()) {
			return false;
		}
		int world = DigHost.world();
		if (world == 0) {
			return false;
		}
		int x = BlockPos.getX(key), y = BlockPos.getY(key), z = BlockPos.getZ(key);
		DugColumn column = COLUMNS.get(ChunkPos.pack(x >> 4, z >> 4));
		return column != null && column.isDug(world, x, y, z);
	}

	public static boolean isDug(int world, BlockPos pos) {
		DugColumn column = COLUMNS.get(ChunkPos.pack(pos.getX() >> 4, pos.getZ() >> 4));
		return column != null && column.isDug(world, pos.getX(), pos.getY(), pos.getZ());
	}

	public static DugColumn column(LevelChunk chunk) {
		DugColumn column = chunk.getAttached(DUG);
		return column != null ? column : DugColumn.EMPTY;
	}

	// ---- start-up ------------------------------------------------------------------------------------------

	private static boolean initialised;

	/** Mod start: the attachment is registered by loading this class; here the filter, packets, events and commands. */
	public static synchronized void init() {
		if (initialised) {
			return;
		}
		initialised = true;
		ChamberBlocks.init();
		SkyCollision.setDugFilter(SkyDig::isDugKey);
		PayloadTypeRegistry.serverboundPlay().register(DigOpen.TYPE, DigOpen.CODEC);
		ServerPlayNetworking.registerGlobalReceiver(DigOpen.TYPE, (payload, context) -> {
			ServerPlayer player = context.player();
			context.server().execute(() -> open(player, payload.world(), payload.pos(), payload.material(), true, "mined"));
		});
		ServerChunkEvents.CHUNK_LOAD.register((level, chunk, generated) -> {
			cleanOnLoad(level, chunk);
			cleanForeignOnLoad(chunk);
			DugColumn column = column(chunk);
			if (!column.isEmpty()) {
				mirror(chunk.getPos().pack(), column);
			}
		});
		ServerLifecycleEvents.SERVER_STOPPED.register(server -> {
			clearMirror();
			PENDING.clear();
			currentWorld = 0;
			CLEAR_PENDING.clear();
			CLEANED.clear();
		});
		// A block of a dug cell broken by a player: the cells around it that are inside the host's geometry become blocks too. (Any other way a
		// block goes is caught by the client's setBlocksDirty hook, ClientLevelDigMixin -> SkyDigClient.blockChanged.)
		PlayerBlockBreakEvents.AFTER.register((level, player, pos, state, blockEntity) -> {
			if (level instanceof ServerLevel serverLevel && destruction) {
				revealAfterBreak(serverLevel, pos, state);
			}
		});
		// Bedrock in a dug cell is the dig limit (or the world's edge): not even a creative player breaks through it.
		PlayerBlockBreakEvents.BEFORE.register((level, player, pos, state, blockEntity) -> {
			if (state.is(Blocks.BEDROCK)) {
				int world = DigHost.world();
				return world == 0 || !isDug(world, pos);
			}
			return true;
		});
		net.fabricmc.fabric.api.event.lifecycle.v1.ServerTickEvents.END_SERVER_TICK.register(SkyDig::tickPending);
		net.fabricmc.fabric.api.event.lifecycle.v1.ServerTickEvents.END_SERVER_TICK.register(SkyDig::tickWorldChange);
		CommandRegistrationCallback.EVENT.register((dispatcher, context, environment) -> registerCommands(dispatcher));
		SkyCraft.LOG.info("Portalcraft: digging registered ({}), chamber blocks {}", DUG.identifier(), ChamberBlocks.NAMESPACE);
	}

	// ---- server --------------------------------------------------------------------------------------------

	private static final double REACH = 8.0;
	private static final AtomicInteger LOGGED = new AtomicInteger(), REVEALED = new AtomicInteger();
	private static final int LOG_CAP = 4000; // open / refused / waiting lines (bridge #38: the 80-line cap of #35 swallowed every line of dig8's into_wall run)
	private static final int REVEAL_LOG_FIRST = 300; // reveals are five or more per mined block: the first 300, then every 25th (with the running count)

	/** The chamber family (CH_*) of the surface last dug, for reveals whose cause has no family of its own (explosions, hand-broken stone). */
	private static volatile int lastFamily = DigMath.CH_PANEL_WHITE;

	private static void log(String what, BlockPos pos, int material, String why) {
		if (what.equals("reveal")) {
			int n = REVEALED.incrementAndGet();
			if (n > REVEAL_LOG_FIRST && n % 25 != 0) {
				return;
			}
			why = why + " [reveal #" + n + "]";
		} else if (LOGGED.incrementAndGet() > LOG_CAP) {
			return;
		}
		SkyCraft.LOG.info("Portalcraft dig: {} cell {} {} {} material {} ({}){}", what, pos.getX(), pos.getY(), pos.getZ(), DigMath.kindName(material), material,
			why.isEmpty() ? "" : " " + why);
	}

	private static boolean inReach(ServerPlayer player, BlockPos pos, double reach) {
		return player.getEyePosition().distanceToSqr(pos.getX() + 0.5, pos.getY() + 0.5, pos.getZ() + 0.5) <= reach * reach;
	}

	/** Marks a cell dug in this world (attachment + mirror). Returns false if it already was. */
	public static boolean markDug(ServerLevel level, int world, BlockPos pos) {
		LevelChunk chunk = level.getChunkAt(pos);
		DugColumn column = column(chunk);
		if (column.isDug(world, pos.getX(), pos.getY(), pos.getZ())) {
			return false;
		}
		DugColumn next = column.with(world, pos.getX(), pos.getY(), pos.getZ());
		CLEANED.add(chunk.getPos().pack()); // (dug since the last /pcdig clear: its cells are new, a later load must not clean them)
		chunk.setAttached(DUG, next);
		chunk.markUnsaved();
		mirror(chunk.getPos().pack(), next);
		return true;
	}

	/**
	 * A player mined the host's surface in this cell. The cell must hold host content and not be at the dig limit; the cell becomes dug
	 * (no block placed), with the drops of the material's block (not in creative), then the cells around it that are inside the host's
	 * geometry become blocks. There is no "void" refusal any more (bridge #38): behind a chamber's wall is deep rock to dig through.
	 * Returns true if the cell was opened. {@code player} may be null (commands); {@code checkReach} false skips the reach check.
	 */
	public static boolean open(@Nullable ServerPlayer player, int world, BlockPos pos, int material, boolean checkReach, String why) {
		ServerLevel level = player != null ? player.level() : null;
		return level != null && open(level, player, world, pos, material, checkReach, why);
	}

	public static boolean open(ServerLevel level, @Nullable ServerPlayer player, int world, BlockPos pos, int material, boolean checkReach, String why) {
		// (bridge #35) every refusal is logged with its reason (dig2: a refused cell looked like "nothing happened")
		if (!destruction || world == 0 || world != DigHost.world() || level.dimension() != Level.OVERWORLD || !level.isLoaded(pos)) {
			log("refused", pos, material, why + ": " + (!destruction ? "destruction is off" : world == 0 || world != DigHost.world() ? "not this map's dig world" : !level.isLoaded(pos) ? "chunk not loaded" : "not the overworld"));
			return false;
		}
		if (player != null && (player.isSpectator() || player.gameMode() == GameType.ADVENTURE || (checkReach && !inReach(player, pos, REACH)))) {
			log("refused", pos, material, why + ": " + (player.isSpectator() || player.gameMode() == GameType.ADVENTURE ? "game mode" : "out of reach"));
			return false;
		}
		if (!SkyCollision.active() || !DigMath.canOpen(SkyCollision.rawHasVisible(pos), SkyCollision.rawLimit(pos) ? DigMath.COL_BLOCK_LIMIT : 0)) {
			log("refused", pos, material, why + ": " + (!SkyCollision.active() ? "no host collision yet" : !SkyCollision.rawHasVisible(pos) ? (SkyCollision.rawHasGeometry(pos) ? "only invisible collision in the cell (nothing to mine)" : "no host content in the cell (air, or its region is not exported)")
				: "dig limit: the edge of what Portal 2 can hold"));
			return false;
		}
		if (level.isOutsideBuildHeight(pos)) {
			log("refused", pos, material, why + ": outside the world's build height");
			return false;
		}
		BlockState here = level.getBlockState(pos);
		if (!here.isAir() && !here.canBeReplaced()) {
			log("refused", pos, material, why + ": a real block is there"); // Minecraft's own breaking handles it
			return false;
		}
		material = DigMath.materialOf(material);
		if (!markDug(level, world, pos)) {
			log("refused", pos, material, why + ": already dug");
			return false;
		}
		if (material != DigMath.CH_NONE) {
			lastFamily = material;
		}
		BlockState state = ChamberBlocks.stateFor(material);
		if (player != null && !player.isCreative()) {
			ItemStack tool = player.getMainHandItem();
			ItemStack used = tool.copy();
			boolean harvest = player.hasCorrectToolForDrops(state);
			tool.mineBlock(level, state, pos, player);
			if (harvest) {
				state.getBlock().playerDestroy(level, player, pos, state, null, used);
			}
		}
		level.levelEvent(LevelEvent.PARTICLES_AND_SOUND_DESTROY_BLOCK, pos, Block.getId(state));
		log("open", pos, material, why);
		revealAround(level, world, pos, material, "reveal after open");
		return true;
	}

	// ---- reveal ---------------------------------------------------------------------------------------------

	/** The host's field as the reveal rule asks for it: the exported voxels (any thread) and the dug set. */
	private static final class HostField implements DigMath.Field {
		final int world;
		final BlockPos.MutableBlockPos scratch = new BlockPos.MutableBlockPos();

		HostField(int world) {
			this.world = world;
		}

		@Override
		public int samples(int x, int y, int z) {
			if (!SkyCollision.isKnown(x, y, z)) {
				return -1; // the host has not sent this region (yet)
			}
			return SkyCollision.visibleSamples(this.scratch.set(x, y, z));
		}

		@Override
		public boolean dug(int x, int y, int z) {
			return isDug(this.world, this.scratch.set(x, y, z));
		}

		@Override
		public boolean limit(int x, int y, int z) {
			return SkyCollision.rawLimit(this.scratch.set(x, y, z));
		}
	}

	/**
	 * SkyCraft's reveal: the face neighbours of a dug cell that are inside the host's geometry (25 of 27 sample points, the void behind
	 * the walls included) become blocks, by depth from the chamber family {@code family} (a CH_*). Cells whose region has not arrived, or
	 * whose chunk is not loaded, are tried again a few times a second for a while. Returns how many became blocks now.
	 */
	public static int revealAround(ServerLevel level, int world, BlockPos center, int family, String why) {
		if (!destruction || world == 0 || !SkyCollision.active()) {
			return 0;
		}
		HostField field = new HostField(world);
		int n = 0;
		// The 6 face neighbours (SkyCraft), and (bridge #38) the 12 edge neighbours: the plugin draws every block face 0.06 u behind its plane (so a portal
		// sits flush on it), and where two walls of a hole meet along an edge with no block behind it that is a gap through which Portal 2's void shows
		// (dig7's shaft_wall: a thin white line at each corner of the shaft). A block in the edge cell closes it.
		for (int dx = -1; dx <= 1; dx++) {
			for (int dy = -1; dy <= 1; dy++) {
				for (int dz = -1; dz <= 1; dz++) {
					int nonZero = (dx != 0 ? 1 : 0) + (dy != 0 ? 1 : 0) + (dz != 0 ? 1 : 0);
					if (nonZero == 0 || nonZero == 3) {
						continue;
					}
					if (revealCell(level, field, center.offset(dx, dy, dz), family, why, true)) {
						n++;
					}
				}
			}
		}
		return n;
	}

	/** One neighbour. Returns true if it became a block (or a dug cell with the block that was already there). */
	private static boolean revealCell(ServerLevel level, HostField field, BlockPos p, int family, String why, boolean queueIfPending) {
		int world = field.world;
		if (isDug(world, p) || level.isOutsideBuildHeight(p)) {
			return false; // (a cell above or below the world's build height stays the host's: the client draws its wall)
		}
		int kind = level.isLoaded(p) ? DigMath.classify(field, p.getX(), p.getY(), p.getZ(), family) : DigMath.PENDING;
		if (kind == DigMath.PENDING) {
			if (queueIfPending && PENDING.size() < 4096) {
				PENDING.add(new Pending(level, world, p.immutable(), family, why, 400));
			}
			return false;
		}
		if (kind == DigMath.AIR) {
			return false;
		}
		if (digCell(level, world, p, kind)) {
			log("reveal", p, kind, why);
			return true;
		}
		return false;
	}

	private record Pending(ServerLevel level, int world, BlockPos pos, int family, String why, int tries) {
	}

	private static final java.util.concurrent.ConcurrentLinkedQueue<Pending> PENDING = new java.util.concurrent.ConcurrentLinkedQueue<>();
	private static int pendingTick;

	/** Server tick: the cells waiting for the host's region or their chunk (a tunnel running into regions the host has not exported yet). */
	private static void tickPending(net.minecraft.server.MinecraftServer server) {
		if (PENDING.isEmpty() || ++pendingTick % 5 != 0) {
			return;
		}
		int budget = PENDING.size();
		while (budget-- > 0) {
			Pending p = PENDING.poll();
			if (p == null) {
				break;
			}
			if (!destruction || p.world != DigHost.world() || isDug(p.world, p.pos)) {
				continue;
			}
			HostField field = new HostField(p.world);
			boolean loaded = p.level.isLoaded(p.pos);
			int kind = loaded ? DigMath.classify(field, p.pos.getX(), p.pos.getY(), p.pos.getZ(), p.family) : DigMath.PENDING;
			if (kind == DigMath.PENDING) {
				if (p.tries > 1) {
					PENDING.add(new Pending(p.level, p.world, p.pos, p.family, p.why, p.tries - 1));
				} else {
					log("gave up on", p.pos, DigMath.CH_NONE, p.why + ": " + (loaded ? "the host never sent its region" : "its chunk is not loaded"));
				}
			} else if (kind != DigMath.AIR && digCell(p.level, p.world, p.pos, kind)) {
				log("reveal", p.pos, kind, p.why + " (was waiting)");
			}
		}
	}

	/** A cell inside the host's geometry: dug out of it, and the block it is made of put there (an existing block stays). */
	public static boolean digCell(ServerLevel level, int world, BlockPos pos, int kind) {
		if (!markDug(level, world, pos)) {
			return false;
		}
		BlockState here = level.getBlockState(pos);
		if (here.isAir() || here.canBeReplaced()) {
			level.setBlock(pos, stateOfKind(level, pos, kind), 3);
		}
		return true;
	}

	/** The block a reveal kind (DigMath.classify) stands for: a chamber block, stone with SkyCraft's ores, or bedrock. */
	public static BlockState stateOfKind(ServerLevel level, BlockPos pos, int kind) {
		return switch (kind) {
			case DigMath.BK_BEDROCK -> Blocks.BEDROCK.defaultBlockState();
			case DigMath.BK_STONE -> stoneOrOre(level.getSeed(), pos);
			default -> ChamberBlocks.stateFor(kind);
		};
	}

	/**
	 * Ores come in little clusters (a 2x2x2 cell picks one ore, then each block in it maybe). From SkyCraft's SkyDig (MIT): the stone
	 * inside the rocks it digs into.
	 */
	static BlockState stoneOrOre(long seed, BlockPos pos) {
		long cell = mix(seed ^ mix(((long) (pos.getX() >> 1) * 73856093L) ^ ((long) (pos.getY() >> 1) * 19349663L) ^ ((long) (pos.getZ() >> 1) * 83492791L)));
		long block = mix(cell ^ ((pos.getX() & 1) | (pos.getY() & 1) << 1 | (pos.getZ() & 1) << 2));
		if ((block & 0xFF) >= 150) { // about 60% of a vein cell's blocks
			return Blocks.STONE.defaultBlockState();
		}
		int roll = (int) ((cell >>> 16) % 10000L);
		// Chances per vein cell, in 1/10000.
		if ((roll -= 330) < 0) {
			return Blocks.COAL_ORE.defaultBlockState();
		}
		if ((roll -= 220) < 0) {
			return Blocks.IRON_ORE.defaultBlockState();
		}
		if ((roll -= 170) < 0) {
			return Blocks.COPPER_ORE.defaultBlockState();
		}
		if ((roll -= 80) < 0) {
			return Blocks.REDSTONE_ORE.defaultBlockState();
		}
		if ((roll -= 45) < 0) {
			return Blocks.GOLD_ORE.defaultBlockState();
		}
		if ((roll -= 35) < 0) {
			return Blocks.LAPIS_ORE.defaultBlockState();
		}
		if ((roll -= 14) < 0) {
			return Blocks.DIAMOND_ORE.defaultBlockState();
		}
		if ((roll -= 5) < 0) {
			return Blocks.EMERALD_ORE.defaultBlockState();
		}
		return Blocks.STONE.defaultBlockState();
	}

	private static long mix(long z) {
		z = (z ^ (z >>> 30)) * 0xBF58476D1CE4E5B9L;
		z = (z ^ (z >>> 27)) * 0x94D049BB133111EBL;
		return z ^ (z >>> 31);
	}

	/**
	 * A block of a dug cell was broken (by a player, an explosion, anything: PlayerBlockBreakEvents and SkyDigClient's block-change hook):
	 * the cells around it that are inside the host's geometry become blocks too, so the way on is closed. The family is the surface's:
	 * the broken block's own if it is a chamber surface block, else one next to it, else the last one dug.
	 */
	public static void revealAfterBreak(ServerLevel level, BlockPos pos, BlockState broken) {
		int world = DigHost.world();
		if (!destruction || world == 0 || !isDug(world, pos)) {
			return;
		}
		revealAround(level, world, pos, familyAround(level, pos, broken), "broke " + broken.getBlock().getName().getString());
	}

	/** The surface family (CH_*) near a cell: the block's own, or a chamber surface block next to it, else the last dug. */
	private static int familyAround(ServerLevel level, BlockPos pos, BlockState state) {
		int own = ChamberBlocks.materialOf(state);
		if (own > 0 && own != DigMath.CH_CONCRETE) {
			return own;
		}
		for (Direction d : Direction.values()) {
			int m = ChamberBlocks.materialOf(level.getBlockState(pos.relative(d)));
			if (m > 0 && m != DigMath.CH_CONCRETE) {
				return m;
			}
		}
		return lastFamily;
	}

	/** The blocks a reveal puts in a dug cell: a chamber block, stone, one of SkyCraft's ores, bedrock. */
	private static boolean isRevealedBlock(BlockState state) {
		return ChamberBlocks.isChamber(state) || state.is(Blocks.STONE) || state.is(Blocks.BEDROCK) || state.is(Blocks.COAL_ORE) || state.is(Blocks.IRON_ORE)
			|| state.is(Blocks.COPPER_ORE) || state.is(Blocks.REDSTONE_ORE) || state.is(Blocks.GOLD_ORE) || state.is(Blocks.LAPIS_ORE) || state.is(Blocks.DIAMOND_ORE)
			|| state.is(Blocks.EMERALD_ORE);
	}

	// Worlds cleared by /pcdig clear (bridge #38): chunks that were not loaded then still hold their dug cells and revealed blocks in their saved data
	// (dig8: a test run found the old tunnel of the run before). Each such chunk is cleaned when it first loads after the clear.
	private static final java.util.Set<Integer> CLEAR_PENDING = ConcurrentHashMap.newKeySet();
	private static final java.util.Set<Long> CLEANED = ConcurrentHashMap.newKeySet(); // chunks handled since the last clear (a chunk dug in later keeps its cells)
	private static int cleanedLate;

	/** Removes this world's dug cells from a chunk, and the blocks a reveal put in them. Returns how many cells. */
	private static int cleanChunk(ServerLevel level, LevelChunk chunk, int world) {
		int cells = cleanChunkWorlds(chunk, w -> w == world);
		if (cells >= 0) {
			CLEANED.add(chunk.getPos().pack());
		}
		return Math.max(cells, 0);
	}

	/** Removes the dug cells of every world the predicate selects from a chunk, and the blocks a reveal put in them. Returns how many cells, or -1 if the chunk held none of those worlds. */
	private static int cleanChunkWorlds(LevelChunk chunk, java.util.function.IntPredicate worlds) {
		DugColumn column = column(chunk);
		if (!column.hasWorldWhere(worlds)) {
			return -1;
		}
		int cx = chunk.getPos().x(), cz = chunk.getPos().z();
		int cells = 0;
		for (DugSection s : column.sections()) {
			if (!worlds.test(s.world()) || s.bits().length != 64) {
				continue;
			}
			for (int bit = 0; bit < 4096; bit++) {
				if (!DigMath.testBit(s.bits(), bit)) {
					continue;
				}
				BlockPos pos = new BlockPos(cx * 16 + (bit & 15), s.sectionY() * 16 + (bit >> 8), cz * 16 + ((bit >> 4) & 15));
				if (isRevealedBlock(chunk.getBlockState(pos))) {
					chunk.setBlockState(pos, Blocks.AIR.defaultBlockState(), 2 | 16);
				}
				cells++;
			}
		}
		DugColumn next = column.withoutWorlds(worlds);
		if (next.isEmpty()) {
			chunk.removeAttached(DUG);
		} else {
			chunk.setAttached(DUG, next);
		}
		chunk.markUnsaved();
		mirror(chunk.getPos().pack(), next);
		return cells;
	}

	// (bridge #46) THE OTHER MAPS' DIGS. The mirror world is one Minecraft world for every map, saved between sessions; a map's dug cells are its own (the dig
	// world id is a hash of the map name) but the blocks a reveal put in them are real blocks of that one world, so the next map found the last map's tunnels as
	// stone, bricks and white panels floating in its open air (the user's "blocks in the middle of the air": the save held the reveals of 10 maps, 661 blocks of
	// bridge_the_gap alone). Only the current map's digs may exist: when the host names a dig world that is not the one before (a new map, the first map of a
	// session), every other world's dug cells and revealed blocks are removed from the loaded chunks, and from the others as they load.
	private static volatile int currentWorld;
	private static final java.util.concurrent.atomic.AtomicInteger FOREIGN_CLEANED = new java.util.concurrent.atomic.AtomicInteger();

	/** Server tick: has the host's dig world changed (a new map)? */
	private static void tickWorldChange(net.minecraft.server.MinecraftServer server) {
		int w = DigHost.world();
		if (w == 0 || w == currentWorld) {
			return;
		}
		currentWorld = w;
		if (dev.skycraft.portalcraft.ChamberSlots.keepsOtherMaps()) {
			// (bridge #78) the unified world keeps every map's digs: the blocks a reveal put in them are stored with their map (ChamberSlots)
			SkyCraft.LOG.info("Portalcraft dig: this map is dig world {}; the other maps' dug cells are kept (their blocks are stored with their maps)", w);
			return;
		}
		ServerLevel level = server.overworld();
		if (level == null) {
			return;
		}
		int cells = 0, chunks = 0;
		for (long key : mirroredChunkKeys()) {
			LevelChunk chunk = level.getChunkSource().getChunkNow(ChunkPos.getX(key), ChunkPos.getZ(key));
			if (chunk == null) {
				continue;
			}
			int n = cleanChunkWorlds(chunk, other -> other != w);
			if (n >= 0) {
				cells += n;
				chunks++;
			}
		}
		FOREIGN_CLEANED.addAndGet(cells);
		SkyCraft.LOG.info("Portalcraft dig: this map is dig world {}: removed the {} dug cells (and the blocks revealed in them) of other maps from {} loaded chunks; chunks that load later are cleaned as they do", w, cells, chunks);
	}

	/** Chunk load: dug cells of another map are removed (the blocks too) once a map's dig world is known. */
	private static void cleanForeignOnLoad(LevelChunk chunk) {
		int w = currentWorld;
		if (w == 0 || dev.skycraft.portalcraft.ChamberSlots.keepsOtherMaps()) {  // (bridge #78)
			return;
		}
		int n = cleanChunkWorlds(chunk, other -> other != w);
		if (n >= 0 && FOREIGN_CLEANED.addAndGet(n) <= 4000) {
			SkyCraft.LOG.info("Portalcraft dig: chunk {} {} held {} dug cells of other maps: cleaned as it loaded", chunk.getPos().x(), chunk.getPos().z(), n);
		}
	}

	/** Removes every dug cell of this world from the loaded chunks (and the blocks a reveal put in them); chunks not loaded now are cleaned when they load. Returns how many cells. */
	public static int clearWorld(ServerLevel level, int world) {
		int cells = 0, chunks = 0;
		CLEANED.clear();
		for (long key : mirroredChunkKeys()) {
			LevelChunk chunk = level.getChunkSource().getChunkNow(ChunkPos.getX(key), ChunkPos.getZ(key));
			if (chunk == null) {
				continue;
			}
			cells += cleanChunk(level, chunk, world);
			chunks++;
		}
		CLEAR_PENDING.add(world);
		SkyCraft.LOG.info("Portalcraft dig: cleared {} dug cells of world {} in {} loaded chunks (chunks that load later are cleaned as they do)", cells, world, chunks);
		return cells;
	}

	/** Chunk load: a chunk that still has the dug cells of a cleared world gets them (and their blocks) removed now. */
	private static void cleanOnLoad(ServerLevel level, LevelChunk chunk) {
		if (CLEAR_PENDING.isEmpty() || !CLEANED.add(chunk.getPos().pack())) {
			return;
		}
		DugColumn column = column(chunk);
		for (int world : CLEAR_PENDING) {
			if (column.hasWorld(world)) {
				int n = cleanChunk(level, chunk, world);
				if (++cleanedLate <= 40) {
					SkyCraft.LOG.info("Portalcraft dig: chunk {} {} held {} dug cells of the cleared world {}: cleaned as it loaded", chunk.getPos().x(), chunk.getPos().z(), n, world);
				}
			}
		}
	}

	// ---- /pcdig (op-only; for tests without mining by hand) -------------------------------------------------

	private static void registerCommands(com.mojang.brigadier.CommandDispatcher<CommandSourceStack> dispatcher) {
		dispatcher.register(Commands.literal("pcdig").requires(Commands.hasPermission(Commands.LEVEL_GAMEMASTERS))
			.then(Commands.literal("open").then(Commands.argument("pos", BlockPosArgument.blockPos())
				.executes(c -> cmdOpen(c, -1))
				.then(Commands.argument("material", IntegerArgumentType.integer(0, 6)).executes(c -> cmdOpen(c, IntegerArgumentType.getInteger(c, "material"))))))
			.then(Commands.literal("reveal").then(Commands.argument("pos", BlockPosArgument.blockPos())
				.executes(c -> cmdReveal(c, -1))
				.then(Commands.argument("material", IntegerArgumentType.integer(0, 6)).executes(c -> cmdReveal(c, IntegerArgumentType.getInteger(c, "material"))))))
			.then(Commands.literal("mine").then(Commands.argument("pos", BlockPosArgument.blockPos()).executes(SkyDig::cmdMine)))
			.then(Commands.literal("info").then(Commands.argument("pos", BlockPosArgument.blockPos()).executes(SkyDig::cmdInfo)))
			.then(Commands.literal("clear").executes(SkyDig::cmdClear))
			.then(Commands.literal("destruction").then(Commands.argument("on", BoolArgumentType.bool()).executes(c -> {
				destruction = BoolArgumentType.getBool(c, "on");
				c.getSource().sendSuccess(() -> Component.literal("Portalcraft digging " + (destruction ? "on" : "off")), false);
				return 1;
			}))));
	}

	private static int cmdOpen(CommandContext<CommandSourceStack> c, int material) throws com.mojang.brigadier.exceptions.CommandSyntaxException {
		CommandSourceStack src = c.getSource();
		BlockPos pos = BlockPosArgument.getLoadedBlockPos(c, "pos");
		int world = DigHost.world();
		if (world == 0) {
			src.sendFailure(Component.literal("pcdig: the host gave no dig world (HP_DIG clear): nothing to open"));
			return 0;
		}
		int m = material >= 0 ? material : DigHost.material();
		boolean ok = open(src.getLevel(), src.getPlayer(), world, pos, m, false, "command");
		src.sendSuccess(() -> Component.literal(ok ? "pcdig: opened " + pos.toShortString() + " as " + DigMath.blockName(m)
			: "pcdig: " + pos.toShortString() + " not opened (no host content / dig limit / already dug / a block is there / destruction off)"), false);
		return ok ? 1 : 0;
	}

	private static int cmdReveal(CommandContext<CommandSourceStack> c, int material) throws com.mojang.brigadier.exceptions.CommandSyntaxException {
		CommandSourceStack src = c.getSource();
		BlockPos pos = BlockPosArgument.getLoadedBlockPos(c, "pos");
		int world = DigHost.world();
		if (world == 0) {
			src.sendFailure(Component.literal("pcdig: the host gave no dig world (HP_DIG clear)"));
			return 0;
		}
		int m = material >= 0 ? material : Math.max(0, ChamberBlocks.materialOf(src.getLevel().getBlockState(pos)));
		int n = revealAround(src.getLevel(), world, pos, m, "command");
		src.sendSuccess(() -> Component.literal("pcdig: revealed " + n + " cells around " + pos.toShortString() + " as " + DigMath.blockName(m)), false);
		return n;
	}

	/** Mines a cell as a player would, for scripted tests: a block there is broken (and the cells around it revealed), else the host's surface in it is opened. */
	private static int cmdMine(CommandContext<CommandSourceStack> c) throws com.mojang.brigadier.exceptions.CommandSyntaxException {
		CommandSourceStack src = c.getSource();
		BlockPos pos = BlockPosArgument.getLoadedBlockPos(c, "pos");
		int world = DigHost.world();
		if (world == 0) {
			src.sendFailure(Component.literal("pcdig: the host gave no dig world (HP_DIG clear)"));
			return 0;
		}
		ServerLevel level = src.getLevel();
		BlockState state = level.getBlockState(pos);
		if (!state.isAir() && !state.canBeReplaced()) {
			if (state.is(Blocks.BEDROCK) && isDug(world, pos)) {
				src.sendFailure(Component.literal("pcdig: bedrock at " + pos.toShortString() + " (the dig limit)"));
				return 0;
			}
			level.setBlock(pos, Blocks.AIR.defaultBlockState(), 3);
			revealAfterBreak(level, pos, state);
			src.sendSuccess(() -> Component.literal("pcdig: broke " + state.getBlock().getName().getString() + " at " + pos.toShortString()), false);
			return 1;
		}
		int m = DigMath.materialOf(DigHost.material());
		boolean ok = open(level, src.getPlayer(), world, pos, m, false, "command mine");
		src.sendSuccess(() -> Component.literal(ok ? "pcdig: opened " + pos.toShortString() + " as " + DigMath.blockName(m) : "pcdig: " + pos.toShortString() + " not opened (see the log)"), false);
		return ok ? 1 : 0;
	}

	/** What the reveal rule says about a cell, for logs and tests. */
	private static int cmdInfo(CommandContext<CommandSourceStack> c) throws com.mojang.brigadier.exceptions.CommandSyntaxException {
		CommandSourceStack src = c.getSource();
		BlockPos pos = BlockPosArgument.getLoadedBlockPos(c, "pos");
		int world = DigHost.world();
		HostField f = new HostField(world);
		int n = f.samples(pos.getX(), pos.getY(), pos.getZ());
		int kind = world == 0 ? DigMath.AIR : DigMath.classify(f, pos.getX(), pos.getY(), pos.getZ(), lastFamily);
		String text = "pcdig info " + pos.toShortString() + ": world " + world + ", " + (isDug(world, pos) ? "DUG" : "not dug") + ", block " + src.getLevel().getBlockState(pos).getBlock().getName().getString()
			+ ", host samples " + (n < 0 ? "unknown (region not sent)" : n + "/27") + (SkyCollision.rawLimit(pos) ? ", DIG LIMIT" : "") + ", depth " + (n >= DigMath.WHOLE ? DigMath.depth(f, pos.getX(), pos.getY(), pos.getZ()) : 0)
			+ ", reveal says " + (kind == DigMath.PENDING ? "wait" : kind == DigMath.AIR ? "stays host geometry" : DigMath.kindName(kind));
		src.sendSuccess(() -> Component.literal(text), false);
		SkyCraft.LOG.info("Portalcraft: {}", text);
		return 1;
	}

	private static int cmdClear(CommandContext<CommandSourceStack> c) {
		CommandSourceStack src = c.getSource();
		int world = DigHost.world();
		if (world == 0) {
			src.sendFailure(Component.literal("pcdig: the host gave no dig world (HP_DIG clear)"));
			return 0;
		}
		int n = clearWorld(src.getLevel(), world);
		src.sendSuccess(() -> Component.literal("pcdig: cleared " + n + " dug cells of world " + world), false);
		return n;
	}

	/** (bridge #53) the sizes of this class's static caches and queues, for the periodic growth line in the log. */
	public static String growth() {
		return "dig columns " + COLUMNS.size() + ", pending " + PENDING.size() + ", clearPending " + CLEAR_PENDING.size() + ", cleaned " + CLEANED.size();
	}
}
