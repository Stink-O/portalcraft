package dev.skycraft.client.mixin;

import dev.skycraft.world.PortalHoles;
import net.minecraft.client.player.LocalPlayer;
import net.minecraft.client.renderer.culling.Frustum;
import net.minecraft.client.renderer.extract.LevelExtractor;
import net.minecraft.world.level.block.state.BlockState;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * Portalcraft (bridge #20): an eye inside an open portal's hole (a portal on Minecraft blocks, the player a tick ahead
 * of Portal 2's transit) is on its way through the portal, not inside the block: no "inside a block" screen overlay.
 * It covered the whole HUD frame with the block's texture for the frames of the crossing (dark frames in c20_transit2f).
 */
@Mixin(LevelExtractor.class)
public abstract class PortalHoleViewMixin {
	@Inject(method = "getViewBlockingState", at = @At("HEAD"), cancellable = true)
	private static void portalcraft$noOverlayInPortalHole(LocalPlayer player, Frustum frustum, CallbackInfoReturnable<BlockState> cir) {
		// (the cells vanilla tests: the eye +-0.4 of the player's width, +-0.05 up; the eye's own centre may still be just in
		// front of the portal's plane while those corners are in the block the portal sits on)
		double r = player.getBbWidth() * 0.4, x = player.getX(), y = player.getEyeY(), z = player.getZ();
		if (PortalHoles.intersects(new net.minecraft.world.phys.AABB(x - r, y - 0.05, z - r, x + r, y + 0.05, z + r))) {
			cir.setReturnValue(null);
		}
	}
}
