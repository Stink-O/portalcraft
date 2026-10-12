package dev.skycraft.client.mixin;

import dev.skycraft.client.portalcraft.PassSectionCache;
import net.minecraft.client.renderer.ViewArea;
import net.minecraft.core.SectionPos;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/** Portalcraft (PassSectionCache): the view area re-assigned its sections to new positions. */
@Mixin(ViewArea.class)
public abstract class PassCacheViewAreaMixin {
	@Inject(method = "repositionCamera", at = @At("RETURN"))
	private void portalcraft$moved(SectionPos cameraSectionPos, CallbackInfoReturnable<Boolean> cir) {
		if (cir.getReturnValueZ()) {
			PassSectionCache.invalidate(1);
		}
	}
}
