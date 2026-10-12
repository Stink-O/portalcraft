package dev.skycraft.client.mixin;

import com.mojang.blaze3d.systems.RenderSystem;
import dev.skycraft.client.portalcraft.HostViewClient;
import dev.skycraft.client.portalcraft.WorldViewExporter;
import net.minecraft.client.DeltaTracker;
import net.minecraft.client.Minecraft;
import net.minecraft.client.TextureFilteringMethod;
import net.minecraft.client.renderer.GameRenderer;
import net.minecraft.client.renderer.state.GameRenderState;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * Portalcraft: grab the level render for the host's camera before the hand and HUD go on top; then,
 * when a v13 host asks for a second camera, render the level again from it in the same frame and
 * grab that too. The second pass re-extracts the camera and the level state for its camera and
 * keeps the primary camera's view area and section graph (SecondPassGuardMixin,
 * SectionOcclusionGraphMixin); its frustum (crop window, shorter render distance) picks the
 * sections it draws.
 */
@Mixin(GameRenderer.class)
public abstract class GameRendererViewMixin {
	private static boolean portalcraft$inSecond;
	private static int portalcraft$secondErrors;

	@Inject(
		method = "renderLevel",
		at = @At(
			value = "INVOKE",
			target = "Lnet/minecraft/client/renderer/GameRenderer;render3dHud(Lnet/minecraft/client/renderer/state/level/CameraRenderState;Lnet/minecraft/client/renderer/state/level/PlayerRenderState;Lnet/minecraft/client/renderer/state/OptionsRenderState;Z)V"
		)
	)
	private void portalcraft$captureLevel(CallbackInfo ci) {
		if (!HostViewClient.active()) {
			// v17: the Minecraft player's HUD starts on transparent (the level stays out of it)
			if (!portalcraft$inSecond) {
				dev.skycraft.client.portalcraft.HudExporter.clearForHud(Minecraft.getInstance());
			}
			return;
		}
		Minecraft minecraft = Minecraft.getInstance();
		// bridge #14: the portal and second-camera passes re-extract the level state for their own cameras, which left the
		// first-person hand's sway inputs (view rotation vs the lagging bob rotation) at a portal camera's angles on every
		// other frame: the hand jumped between two poses. They are the primary camera's again for the hand pass.
		var hands = ((GameRenderer) (Object) this).gameRenderState().levelRenderState.playerRenderState.firstPersonHandsAndItems;
		final float vx = hands.viewXRot, vy = hands.viewYRot, bx = hands.xBob, by = hands.yBob;
		if (!portalcraft$inSecond) {
			var gs = ((GameRenderer) (Object) this).gameRenderState().levelRenderState;
			var av = gs.playerRenderState.avatarRenderState;
			dev.skycraft.client.portalcraft.PlayerTransit.swayFrame(vx, vy, bx, by, gs.cameraRenderState.entityRenderState.bob,
				gs.cameraRenderState.entityRenderState.backwardsInterpolatedWalkDistance, gs.cameraRenderState.entityRenderState.hurtTime,
				hands.mainHandHeight, av == null ? 0f : av.swingAnimation, av == null ? 0f : av.xRot, gs.cameraRenderState.hudFov);
		}
		try {
			portalcraft$captureHostViews(minecraft);
		} finally {
			var h = ((GameRenderer) (Object) this).gameRenderState().levelRenderState.playerRenderState.firstPersonHandsAndItems;
			h.viewXRot = vx;
			h.viewYRot = vy;
			h.xBob = bx;
			h.yBob = by;
			// v17: after every capture of this frame (the second pass's included), the hand and GUI go on transparent
			if (!portalcraft$inSecond) {
				dev.skycraft.client.portalcraft.HudExporter.clearForHud(minecraft);
			}
		}
	}

	private void portalcraft$captureHostViews(Minecraft minecraft) {
		if (!portalcraft$inSecond) {
			// v15: the live pair's views drawn into this frame first (PortalPasses)
			dev.skycraft.client.portalcraft.PortalPasses.frame(minecraft, (GameRenderer) (Object) this);
		}
		WorldViewExporter.capture(minecraft);
		if (portalcraft$inSecond || !HostViewClient.secondActive()) {
			return;
		}
		portalcraft$inSecond = true;
		try {
			portalcraft$secondPass(minecraft, (GameRenderer) (Object) this);
		} catch (RuntimeException e) {
			if (portalcraft$secondErrors++ < 5) {
				dev.skycraft.SkyCraft.LOG.error("Portalcraft: second camera pass failed", e);
			}
		} finally {
			HostViewClient.setPass(0);
			portalcraft$inSecond = false;
		}
	}

	/** v17: the hand pass bobs with the Minecraft player's walk even while the host drives the camera (HostViewNoBobMixin). */
	@Inject(method = "render3dHud", at = @At("HEAD"))
	private void portalcraft$handPassBegin(CallbackInfo ci) {
		dev.skycraft.client.portalcraft.HudExporter.setHandPass(true);
	}

	@Inject(method = "render3dHud", at = @At("RETURN"))
	private void portalcraft$handPassEnd(CallbackInfo ci) {
		dev.skycraft.client.portalcraft.HudExporter.setHandPass(false);
	}

	/** Host views show no block outline: the hidden player's crosshair target is not the host's. */
	@Inject(method = "shouldRenderBlockOutline", at = @At("HEAD"), cancellable = true)
	private void portalcraft$noOutline(CallbackInfoReturnable<Boolean> cir) {
		if (HostViewClient.active()) {
			cir.setReturnValue(false);
		}
	}

	private static void portalcraft$secondPass(Minecraft minecraft, GameRenderer renderer) {
		GameRendererAccessor acc = (GameRendererAccessor) renderer;
		DeltaTracker dt = minecraft.getDeltaTracker();
		GameRenderState st = renderer.gameRenderState();
		float partial = st.levelRenderState.worldPartialTicks;
		HostViewClient.setPass(1);
		// camera (CameraHostViewMixin puts the second camera in), its render state, then the level state
		renderer.mainCamera().update(dt);
		acc.portalcraft$extractCamera(dt, partial);
		((SectionOcclusionGraphAccessor) minecraft.levelRenderer.sectionOcclusionGraph()).portalcraft$needsFrustumUpdate().set(true);
		minecraft.levelExtractor.extract(dt, renderer.mainCamera(), partial);
		// the frame-wide uniforms that carry the camera position, and a fresh fog buffer
		acc.portalcraft$globalSettings().update(st.windowRenderState.width, st.windowRenderState.height, st.optionsRenderState.glintStrength,
			st.levelRenderState.gameTime, partial, st.optionsRenderState.menuBackgroundBlurriness, st.levelRenderState.cameraRenderState.pos,
			st.optionsRenderState.textureFiltering == TextureFilteringMethod.RGSS);
		acc.portalcraft$fogRenderer().endFrame();
		var target = renderer.mainRenderTarget();
		RenderSystem.getDevice().createCommandEncoder().clearColorAndDepthTextures(target.getColorTexture(), st.guiRenderState.clearColorOverride, target.getDepthTexture(), 0.0);
		// the level again (with its own clouds: the cloud buffer is written once per pass); this
		// injection runs inside it once more and captures the second camera
		dev.skycraft.client.portalcraft.PortalPasses.withOwnClouds(dev.skycraft.client.portalcraft.PortalPasses.SECOND, renderer::renderLevel);
		// the next frame's primary pass rebuilds its visible sections for its own frustum
		((SectionOcclusionGraphAccessor) minecraft.levelRenderer.sectionOcclusionGraph()).portalcraft$needsFrustumUpdate().set(true);
	}
}
