package dev.skycraft.mixin;

import dev.skycraft.world.SkyMovers;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.entity.MoverType;
import net.minecraft.world.phys.Vec3;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.ModifyVariable;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/**
 * Portalcraft (bridge #26): every entity that moves itself (the player, mobs, items, arrows) collides with the host's movers
 * (SkyMovers), is carried by the one it stands on and pushed by one that moves into it, before vanilla collides the rest of
 * its movement with blocks and the host's static field. The server's copy of a player is left alone (its client moves it).
 */
@Mixin(Entity.class)
public abstract class EntityMoverMixin {
	@ModifyVariable(method = "move", at = @At("HEAD"), argsOnly = true)
	private Vec3 portalcraft$movers(Vec3 movement) {
		return SkyMovers.beforeMove((Entity) (Object) this, movement);
	}

	@Inject(method = "move", at = @At("RETURN"))
	private void portalcraft$moversGround(MoverType type, Vec3 movement, CallbackInfo ci) {
		SkyMovers.afterMove((Entity) (Object) this);
	}
}
