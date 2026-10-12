package dev.skycraft.world;

import java.util.HashSet;
import java.util.Set;
import net.fabricmc.fabric.api.creativetab.v1.CreativeModeTabEvents;
import net.minecraft.core.Registry;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.core.registries.Registries;
import net.minecraft.resources.Identifier;
import net.minecraft.resources.ResourceKey;
import net.minecraft.world.item.BlockItem;
import net.minecraft.world.item.CreativeModeTabs;
import net.minecraft.world.item.Item;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.SoundType;
import net.minecraft.world.level.block.TransparentBlock;
import net.minecraft.world.level.block.state.BlockBehaviour;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.material.MapColor;
import org.jspecify.annotations.Nullable;

/**
 * Portalcraft (bridge #33): the blocks a Portal 2 chamber surface turns into when it is dug. They reuse vanilla block
 * textures through their model JSON (white/black/gray concrete, iron block, smooth stone, stained glass); nothing from
 * Portal 2 is in the repo. Mapping from the host's ChamberMaterial: see {@link DigMath#BLOCK_NAMES}.
 *
 * Portals stick to the white panel and the tile ({@link #notPortalable}); WorldExporter puts the others in the "not
 * portalable" bitset it already sends after the solids.
 */
public final class ChamberBlocks {
	private ChamberBlocks() {
	}

	public static final String NAMESPACE = "portalcraft";

	private static final Block[] BY_MATERIAL = new Block[DigMath.CH_CONCRETE + 1];
	private static final Set<Block> ALL = new HashSet<>();
	private static final Set<Block> NOT_PORTALABLE = new HashSet<>();
	private static boolean registered;

	public static Block PANEL_WHITE, PANEL_BLACK, METAL, TILE, GLASS, CONCRETE;

	/** Registers the six blocks and their items (once, at mod initialisation). */
	public static synchronized void init() {
		if (registered) {
			return;
		}
		registered = true;
		PANEL_WHITE = register("chamber_panel_white", MapColor.SNOW, SoundType.STONE, 1.5F, true, false);
		PANEL_BLACK = register("chamber_panel_black", MapColor.COLOR_BLACK, SoundType.STONE, 1.5F, false, false);
		METAL = register("chamber_metal", MapColor.METAL, SoundType.IRON, 3.0F, false, false);
		TILE = register("chamber_tile", MapColor.STONE, SoundType.STONE, 1.5F, true, false);
		GLASS = register("chamber_glass", MapColor.NONE, SoundType.GLASS, 0.3F, false, true);
		CONCRETE = register("chamber_concrete", MapColor.COLOR_GRAY, SoundType.STONE, 1.5F, false, false);
		BY_MATERIAL[DigMath.CH_NONE] = PANEL_WHITE;
		BY_MATERIAL[DigMath.CH_PANEL_WHITE] = PANEL_WHITE;
		BY_MATERIAL[DigMath.CH_PANEL_BLACK] = PANEL_BLACK;
		BY_MATERIAL[DigMath.CH_METAL] = METAL;
		BY_MATERIAL[DigMath.CH_TILE] = TILE;
		BY_MATERIAL[DigMath.CH_GLASS] = GLASS;
		BY_MATERIAL[DigMath.CH_CONCRETE] = CONCRETE;
		CreativeModeTabEvents.modifyOutputEvent(CreativeModeTabs.BUILDING_BLOCKS).register(entries -> {
			for (Block b : new Block[] {PANEL_WHITE, PANEL_BLACK, METAL, TILE, GLASS, CONCRETE}) {
				entries.accept(b);
			}
		});
	}

	private static Block register(String name, MapColor color, SoundType sound, float hardness, boolean portalable, boolean glass) {
		Identifier id = Identifier.fromNamespaceAndPath(NAMESPACE, name);
		ResourceKey<Block> blockKey = ResourceKey.create(Registries.BLOCK, id);
		BlockBehaviour.Properties props = BlockBehaviour.Properties.of().mapColor(color).sound(sound).strength(hardness, 6.0F).setId(blockKey);
		if (glass) {
			props = props.noOcclusion().isValidSpawn((s, l, p, t) -> false).isRedstoneConductor((s, l, p) -> false)
				.isSuffocating((s, l, p) -> false).isViewBlocking((s, l, p, a) -> false);
		} else {
			props = props.requiresCorrectToolForDrops();
		}
		Block block = glass ? new TransparentBlock(props) : new Block(props);
		Registry.register(BuiltInRegistries.BLOCK, blockKey, block);
		ResourceKey<Item> itemKey = ResourceKey.create(Registries.ITEM, id);
		BlockItem item = new BlockItem(block, new Item.Properties().setId(itemKey).useBlockDescriptionPrefix());
		item.registerBlocks(Item.BY_BLOCK, item);
		Registry.register(BuiltInRegistries.ITEM, itemKey, item);
		ALL.add(block);
		if (!portalable) {
			NOT_PORTALABLE.add(block);
		}
		return block;
	}

	/** The block for a HostPlayer.digMaterial / CH_* value (low byte; unknown and 0 are the white panel). */
	public static Block blockFor(int material) {
		init();
		return BY_MATERIAL[DigMath.materialOf(material)];
	}

	public static BlockState stateFor(int material) {
		return blockFor(material).defaultBlockState();
	}

	/** True for any of the six chamber blocks. */
	public static boolean isChamber(BlockState state) {
		return ALL.contains(state.getBlock());
	}

	/** The CH_* value of a chamber block (1..6), or -1 if it is not one. */
	public static int materialOf(BlockState state) {
		Block b = state.getBlock();
		if (!ALL.contains(b)) {
			return -1;
		}
		for (int ch = 1; ch < BY_MATERIAL.length; ch++) {
			if (BY_MATERIAL[ch] == b) {
				return ch;
			}
		}
		return -1;
	}

	/** Chamber blocks portals may not be placed on (everything but the white panel and the tile). */
	public static boolean notPortalable(@Nullable BlockState state) {
		return state != null && NOT_PORTALABLE.contains(state.getBlock());
	}
}
