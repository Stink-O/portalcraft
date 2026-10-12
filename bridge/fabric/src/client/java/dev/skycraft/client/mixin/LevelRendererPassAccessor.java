package dev.skycraft.client.mixin;

import net.minecraft.client.renderer.CloudRenderer;
import net.minecraft.client.renderer.LevelRenderer;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Mutable;
import org.spongepowered.asm.mixin.gen.Accessor;

/** Portalcraft: each extra level pass in a frame draws its clouds with its own CloudRenderer (PortalPasses). */
@Mixin(LevelRenderer.class)
public interface LevelRendererPassAccessor {
	@Accessor("cloudRenderer")
	CloudRenderer portalcraft$cloudRenderer();

	@Mutable
	@Accessor("cloudRenderer")
	void portalcraft$setCloudRenderer(CloudRenderer renderer);
}
