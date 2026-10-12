package dev.skycraft.mixin;

import dev.skycraft.world.PortalHoles;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.entity.player.Player;
import net.minecraft.world.phys.AABB;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * Portalcraft (bridge #20): a player whose eye is in an open portal's hole is going through the portal, not stuck in the
 * block the portal sits on: no suffocation ("in wall") damage. The user: "he gets hurt going through the portals".
 */
@Mixin(Entity.class)
public abstract class PortalTransitSafetyMixin {
	@Inject(method = "isInWall", at = @At("HEAD"), cancellable = true)
	private void portalcraft$notInWallInPortalHole(CallbackInfoReturnable<Boolean> cir) {
		Entity self = (Entity) (Object) this;
		double w = self.getBbWidth() * 0.4, x = self.getX(), y = self.getEyeY(), z = self.getZ();
		AABB eye = new AABB(x - w, y - 1e-3, z - w, x + w, y + 1e-3, z + w);
		if (self instanceof Player) {
			if (PortalHoles.intersects(eye)) {
				cir.setReturnValue(false);
			}
			return;
		}
		// (bridge #22) a mob or item half through a linked portal (its head in the blocks behind the opening): not in a wall
		for (int i = 0; i < dev.skycraft.world.PortalPair.SLOTS; i++) {
			AABB h = dev.skycraft.world.PortalPair.hole(i);
			if (h != null && h.intersects(eye.inflate(1e-4))) {
				cir.setReturnValue(false);
				return;
			}
		}
	}
}
