package dev.skycraft.client.mixin;

import dev.skycraft.client.portalcraft.PassSectionCache;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/** Portalcraft (PassSectionCache): a section's mesh changed, or it was reset or moved to another position. */
@Mixin(targets = "net.minecraft.client.renderer.chunk.SectionRenderDispatcher$RenderSection")
public abstract class PassCacheSectionMixin {
	@Inject(method = "setSectionMesh", at = @At("HEAD"))
	private void portalcraft$mesh(CallbackInfoReturnable<?> cir) {
		PassSectionCache.invalidate(2);
	}

	@Inject(method = "reset", at = @At("HEAD"))
	private void portalcraft$reset(CallbackInfo ci) {
		PassSectionCache.invalidate(3);
	}

	@Inject(method = "setSectionNode", at = @At("HEAD"))
	private void portalcraft$node(long sectionNode, CallbackInfo ci) {
		PassSectionCache.invalidate(4);
	}
}
