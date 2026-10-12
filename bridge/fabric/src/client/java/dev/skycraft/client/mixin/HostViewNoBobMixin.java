package dev.skycraft.client.mixin;

import dev.skycraft.client.portalcraft.HostViewClient;
import net.minecraft.client.renderer.GameRenderer;
import net.minecraft.client.renderer.state.level.CameraRenderState;
import com.mojang.blaze3d.vertex.PoseStack;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/**
 * Portalcraft (bridge #11): while the host drives the camera, Minecraft's walk bob and hurt tilt stay out
 * of the projection. GameRenderer.renderLevel multiplies both into the level's projection from the hidden
 * player's own state (options.txt has bobView true); the host's camera is authoritative ("each player's
 * rules travel with them"), and any bob of the hidden player (it is re-placed after Chell every 12 blocks)
 * would move Minecraft's whole image against Portal 2's while walking.
 */
@Mixin(GameRenderer.class)
public abstract class HostViewNoBobMixin {
	@Inject(method = "bobView", at = @At("HEAD"), cancellable = true)
	private void portalcraft$noBobView(CameraRenderState cameraState, PoseStack poseStack, CallbackInfo ci) {
		if (HostViewClient.active() && !dev.skycraft.client.portalcraft.HudExporter.handPass()) {
			ci.cancel();
		}
	}

	@Inject(method = "bobHurt", at = @At("HEAD"), cancellable = true)
	private void portalcraft$noBobHurt(CameraRenderState cameraState, PoseStack poseStack, CallbackInfo ci) {
		if (HostViewClient.active() && !dev.skycraft.client.portalcraft.HudExporter.handPass()) {
			ci.cancel();
		}
	}
}
