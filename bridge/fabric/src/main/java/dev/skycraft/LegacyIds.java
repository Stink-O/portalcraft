package dev.skycraft;

import java.util.List;
import net.fabricmc.fabric.api.event.registry.FabricRegistry;
import net.minecraft.core.Registry;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.resources.Identifier;

/**
 * The mod id was renamed from {@code skycraft} to {@code portalcraft} (2026-10-10). Worlds saved before that still name
 * {@code skycraft:*} entries: Fabric's registry sync (data/fabric/registry_entries.dat) finds them missing and stops on the
 * "backup your world" screen, and the dug-blocks attachment saved as {@code skycraft:dug} in the chunks would be dropped.
 * This registers the old ids as aliases of the new entries and maps the old attachment id for the chunk reader.
 */
public final class LegacyIds {
	private LegacyIds() {
	}

	public static final String OLD_NAMESPACE = "skycraft";

	/** Paths of everything this mod registers into a vanilla registry (blocks, items, entity types). */
	private static final List<String> CHAMBER_BLOCKS = List.of(
		"chamber_panel_white", "chamber_panel_black", "chamber_metal", "chamber_tile", "chamber_glass", "chamber_concrete");
	private static final String SKYRIM_ACTOR = "skyrim_actor";

	/** The new id for a legacy {@code skycraft:*} one, or the id itself when it is not legacy. */
	public static Identifier remap(Identifier id) {
		if (id != null && OLD_NAMESPACE.equals(id.getNamespace())) {
			return Identifier.fromNamespaceAndPath(SkyCraft.MOD_ID, id.getPath());
		}
		return id;
	}

	/** Registers the aliases; call after every registration of the mod's entries. */
	public static void registerAliases() {
		for (String name : CHAMBER_BLOCKS) {
			alias(BuiltInRegistries.BLOCK, name);
			alias(BuiltInRegistries.ITEM, name);
		}
		alias(BuiltInRegistries.ENTITY_TYPE, SKYRIM_ACTOR);
	}

	private static void alias(Registry<?> registry, String path) {
		((FabricRegistry) registry).addAlias(Identifier.fromNamespaceAndPath(OLD_NAMESPACE, path), Identifier.fromNamespaceAndPath(SkyCraft.MOD_ID, path));
	}
}
