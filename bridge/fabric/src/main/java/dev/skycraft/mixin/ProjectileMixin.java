package dev.skycraft.mixin;

import dev.skycraft.world.SkyClip;
import dev.skycraft.world.SkyCollision;
import net.minecraft.world.entity.projectile.Projectile;
import net.minecraft.world.entity.projectile.ProjectileDeflection;
import net.minecraft.world.phys.BlockHitResult;
import net.minecraft.world.phys.HitResult;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * Minecraft ignores projectile hits on air, and to Minecraft a Skyrim wall (or a Portal 2 chamber wall, floor or mover) is air. Treat a hit on
 * host geometry as a real hit: arrows stick in it, snowballs and eggs break on it, fireballs explode on it.
 * <p>
 * (Bridge #48) Which hits those are: a hit on Skyrim's triangles or a host mover (SkyrimHitResult), or one on a cell with host voxels (the
 * collider clip of ClipContextMixin). Both say what they hit; vanilla finds no solid shape in an air cell, so a block hit on air is ours.
 */
@Mixin(Projectile.class)
public abstract class ProjectileMixin {
	@Shadow
	protected abstract void onHit(HitResult hitResult);

	@Inject(method = "hitTargetOrDeflectSelf", at = @At("HEAD"), cancellable = true)
	private void skycraft$hitSkyrim(HitResult hitResult, CallbackInfoReturnable<ProjectileDeflection> cir) {
		if (hitResult instanceof SkyClip.SkyrimHitResult
			|| hitResult instanceof BlockHitResult block && block.getType() == HitResult.Type.BLOCK && !block.isWorldBorderHit() && SkyCollision.active()
				&& SkyCollision.shapeAt(block.getBlockPos()) != null
				&& ((Projectile) (Object) this).level().getBlockState(block.getBlockPos()).isAir()) {
			this.onHit(hitResult);
			cir.setReturnValue(ProjectileDeflection.NONE);
		}
	}
}
