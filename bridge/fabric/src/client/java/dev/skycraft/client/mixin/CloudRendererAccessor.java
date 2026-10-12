package dev.skycraft.client.mixin;

import net.minecraft.client.renderer.CloudRenderer;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.gen.Accessor;

/** Portalcraft: a pass's own CloudRenderer shares the main one's cloud texture (PortalPasses). */
@Mixin(CloudRenderer.class)
public interface CloudRendererAccessor {
	@Accessor("texture")
	CloudRenderer.TextureData portalcraft$texture();

	@Accessor("texture")
	void portalcraft$setTexture(CloudRenderer.TextureData texture);
}
