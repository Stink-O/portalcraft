package dev.skycraft.client.mixin;

import com.llamalad7.mixinextras.injector.wrapoperation.Operation;
import com.llamalad7.mixinextras.injector.wrapoperation.WrapOperation;
import dev.skycraft.client.portalcraft.AvatarHost;
import net.minecraft.client.Camera;
import net.minecraft.client.renderer.extract.LevelExtractor;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;

/**
 * Portalcraft (bridge #16): in host-view passes Minecraft draws its own player only where the host wants it: the body
 * in the region, seen from a camera that is not its eyes (third person, portal views). See {@link AvatarHost}.
 */
@Mixin(LevelExtractor.class)
public abstract class LocalPlayerHostViewMixin {
	@WrapOperation(
		method = "extractVisibleEntities",
		at = @At(value = "INVOKE", target = "Lnet/minecraft/client/Camera;isDetached()Z")
	)
	private boolean portalcraft$localPlayerInHostView(Camera camera, Operation<Boolean> original) {
		return AvatarHost.drawLocalPlayer(camera, original.call(camera));
	}
}
