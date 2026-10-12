package dev.skycraft.client.mixin;

import dev.skycraft.client.portalcraft.PassSectionCache;
import java.util.List;
import java.util.Map;
import net.minecraft.client.renderer.DynamicGpuData;
import net.minecraft.client.renderer.LevelRenderer;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/** Portalcraft (experiment, PassSectionCache): a pass whose visible list was reused reuses its terrain draw groups. */
@Mixin(LevelRenderer.class)
public abstract class PassCacheRendererMixin {
	@SuppressWarnings("rawtypes")
	@Inject(method = "extractSectionDrawGroups", at = @At("HEAD"), cancellable = true)
	private void portalcraft$reuseGroups(boolean respectTranslucentOrder, List<DynamicGpuData.ChunkSectionInfo> sectionInfos, Map drawGroups, CallbackInfoReturnable<Integer> cir) {
		int largest = PassSectionCache.fillGroups(respectTranslucentOrder, sectionInfos, drawGroups);
		if (largest >= 0) {
			cir.setReturnValue(largest);
		}
	}

	@SuppressWarnings("rawtypes")
	@Inject(method = "extractSectionDrawGroups", at = @At("RETURN"))
	private void portalcraft$keepGroups(boolean respectTranslucentOrder, List<DynamicGpuData.ChunkSectionInfo> sectionInfos, Map drawGroups, CallbackInfoReturnable<Integer> cir) {
		PassSectionCache.storeGroups(respectTranslucentOrder, sectionInfos, drawGroups, cir.getReturnValueI());
	}

	/**
	 * Translucent sections are re-sorted for the camera of every pass, every frame (each pass's camera differs from
	 * the last one's), which re-allocates their index buffers continuously and so never leaves the draw groups
	 * unchanged. With the cache on, only the primary camera schedules re-sorts (portal views keep its order).
	 */
	@Inject(method = "scheduleTranslucentSectionResort", at = @At("HEAD"), cancellable = true)
	private void portalcraft$primaryResortsOnly(CallbackInfo ci) {
		if (PassSectionCache.ENABLED && dev.skycraft.client.portalcraft.HostViewClient.pass() != 0) {
			ci.cancel();
		}
	}

	@Inject(method = "clearVisibleSections", at = @At("HEAD"))
	private void portalcraft$listCleared(CallbackInfo ci) {
		PassSectionCache.listCleared();
	}
}
