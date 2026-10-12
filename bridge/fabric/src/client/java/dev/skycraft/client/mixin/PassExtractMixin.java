package dev.skycraft.client.mixin;

import dev.skycraft.client.portalcraft.HostViewClient;
import net.minecraft.client.renderer.extract.LevelExtractor;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Unique;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/**
 * Portalcraft (frame-rate experiment, off by default): the extra level passes (the second camera and the
 * portal views drawn inside Minecraft) can skip entity and block-entity extraction, which every pass repeats
 * for its own camera. -Dportalcraft.passEntities=false turns it on (through PORTALCRAFT_JVM_ARGS); mobs and
 * chests are then missing in portal views, so it is only for measuring what that work costs.
 */
@Mixin(LevelExtractor.class)
public abstract class PassExtractMixin {
	@Unique private static final boolean PASS_ENTITIES = !"false".equals(System.getProperty("portalcraft.passEntities"));

	@Inject(method = "extractVisibleEntities", at = @At("HEAD"), cancellable = true, require = 0)
	private void portalcraft$skipEntities(CallbackInfo ci) {
		if (!PASS_ENTITIES && HostViewClient.pass() >= 1) {
			ci.cancel();
		}
	}

	@Inject(method = "extractVisibleBlockEntities", at = @At("HEAD"), cancellable = true, require = 0)
	private void portalcraft$skipBlockEntities(CallbackInfo ci) {
		if (!PASS_ENTITIES && HostViewClient.pass() >= 1) {
			ci.cancel();
		}
	}
}
