package dev.skycraft.client.mixin;

import dev.skycraft.client.SkyClient;
import net.minecraft.client.renderer.LevelRenderer;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/**
 * Skyrim draws the world. While linked, Minecraft renders nothing of its own level (no sky,
 * clouds, fog or terrain) so the overlay is just hand + HUD on a transparent background.
 */
@Mixin(LevelRenderer.class)
public abstract class LevelRendererMixin {
	private static final boolean TERRAIN_WORLD = "terrain".equals(System.getProperty("skycraft.world"));

	@Inject(
		method = "render(Lcom/mojang/blaze3d/resource/GraphicsResourceAllocator;ZLnet/minecraft/client/renderer/state/level/CameraRenderState;Lcom/mojang/renderpearl/api/buffers/GpuBufferSlice;Lorg/joml/Vector4f;ZZ)V",
		at = @At("HEAD"),
		cancellable = true
	)
	private void skycraft$skipLevel(CallbackInfo ci) {
		// Portalcraft: never in a terrain world. The host composites Minecraft's own frame there
		// (HostViewClient), and skipping the level render would drop the section compile requests
		// the level extractor hands it (they are consumed either way), so the terrain would be
		// missing whenever the host turns its view on later.
		if (SkyClient.linked() && !TERRAIN_WORLD && !dev.skycraft.client.portalcraft.HostViewClient.active()) {
			ci.cancel();
		}
	}
}
