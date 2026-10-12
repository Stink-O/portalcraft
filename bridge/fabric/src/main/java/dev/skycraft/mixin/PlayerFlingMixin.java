package dev.skycraft.mixin;

import dev.skycraft.portalcraft.PlayerFling;
import net.minecraft.world.entity.LivingEntity;
import net.minecraft.world.phys.Vec3;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.ModifyVariable;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * Portalcraft (bridge #28): a flinging player (PlayerFling: from a portal transit until it lands) moves through the air as Portal 2
 * moves Chell: Portal 2's air control before the move, Portal 2's gravity, no air drag.
 */
@Mixin(LivingEntity.class)
public abstract class PlayerFlingMixin {
	@Inject(method = "travel", at = @At("HEAD"))
	private void portalcraft$flingAirMove(Vec3 input, CallbackInfo ci) {
		if (PlayerFling.flinging(this)) {
			LivingEntity self = (LivingEntity) (Object) this;
			PlayerFling.airMove(self, new Vec3(self.xxa, self.yya, self.zza));  // (the input itself: the argument may be zeroed already)
		}
	}

	// (Portal 2's control was applied above: Minecraft's own input acceleration would add up with no friction)
	@ModifyVariable(method = "travel", at = @At("HEAD"), argsOnly = true)
	private Vec3 portalcraft$flingNoMcInput(Vec3 input) {
		return PlayerFling.flinging(this) ? Vec3.ZERO : input;
	}

	@Inject(method = "getEffectiveGravity", at = @At("HEAD"), cancellable = true)
	private void portalcraft$flingGravity(CallbackInfoReturnable<Double> cir) {
		if (PlayerFling.flinging(this)) {
			cir.setReturnValue(PlayerFling.gravity());
		}
	}

	@Inject(method = "shouldDiscardFriction", at = @At("HEAD"), cancellable = true)
	private void portalcraft$flingNoDrag(CallbackInfoReturnable<Boolean> cir) {
		if (PlayerFling.flinging(this)) {
			cir.setReturnValue(true);
		}
	}
}
