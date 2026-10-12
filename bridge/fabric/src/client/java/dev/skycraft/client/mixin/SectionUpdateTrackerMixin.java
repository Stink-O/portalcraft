package dev.skycraft.client.mixin;

import dev.skycraft.client.portalcraft.HostViewClient;
import net.minecraft.client.SectionUpdateTracker;
import net.minecraft.core.SectionPos;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/** Portalcraft: the section update tracker stays centred on the primary camera (second and portal passes). */
@Mixin(SectionUpdateTracker.class)
public abstract class SectionUpdateTrackerMixin {
	@Inject(method = "repositionCamera", at = @At("HEAD"), cancellable = true)
	private void portalcraft$keepCentre(SectionPos cameraSectionPos, CallbackInfo ci) {
		if (HostViewClient.pass() != 0) {
			ci.cancel();
		}
	}
}
