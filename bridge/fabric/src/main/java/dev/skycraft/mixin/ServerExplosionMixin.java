package dev.skycraft.mixin;

import com.llamalad7.mixinextras.injector.wrapoperation.Operation;
import com.llamalad7.mixinextras.injector.wrapoperation.WrapOperation;
import dev.skycraft.link.Proto;
import dev.skycraft.link.SkyLink;
import dev.skycraft.world.SkyClip;
import dev.skycraft.world.SkyCollision;
import java.util.Optional;
import net.minecraft.core.BlockPos;
import net.minecraft.world.level.BlockGetter;
import net.minecraft.world.level.ClipContext;
import net.minecraft.world.level.Explosion;
import net.minecraft.world.level.ExplosionDamageCalculator;
import net.minecraft.world.level.Level;
import net.minecraft.world.level.ServerExplosion;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.material.FluidState;
import net.minecraft.world.phys.BlockHitResult;
import net.minecraft.world.phys.HitResult;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * Tells Skyrim about every Minecraft explosion (TNT, creepers, beds, ...) once it has gone off, so
 * Skyrim's own physics feel it: loose objects are thrown and people are knocked away.
 * <p>
 * Portalcraft (bridge #20): the host's world is solid to the blast. Portal 2's chamber walls are not Minecraft blocks
 * (air to vanilla), so a blast went straight through them: TNT broke placed blocks on the far side of a wall and hurt
 * players behind it. Now a cell that is mostly host geometry (the CollisionField voxels, SkyCollision) resists like
 * bedrock, so the blast's rays stop there and the wall itself never breaks (it is not a block; the user's rule: Portal
 * 2's walls don't break); and an entity's exposure ray that meets host geometry is blocked, as a Minecraft wall would.
 */
@Mixin(ServerExplosion.class)
public abstract class ServerExplosionMixin {
	/** Bedrock's resistance: nothing gets through. */
	private static final float HOST_RESISTANCE = 3_600_000.0F;

	@Inject(method = "explode", at = @At("RETURN"))
	private void skycraft$tellSkyrim(CallbackInfoReturnable<Integer> cir) {
		if (!SkyLink.active()) {
			return;
		}
		ServerExplosion self = (ServerExplosion) (Object) this;
		var center = self.center();
		SkyLink.pushEvent(Proto.EV_EXPLOSION, 0, (float) center.x, (float) center.y, (float) center.z, self.radius(), 0);
	}

	@WrapOperation(
		method = "calculateExplodedPositions",
		at = @At(
			value = "INVOKE",
			target = "Lnet/minecraft/world/level/ExplosionDamageCalculator;getBlockExplosionResistance(Lnet/minecraft/world/level/Explosion;Lnet/minecraft/world/level/BlockGetter;Lnet/minecraft/core/BlockPos;Lnet/minecraft/world/level/block/state/BlockState;Lnet/minecraft/world/level/material/FluidState;)Ljava/util/Optional;"
		)
	)
	private Optional<Float> skycraft$hostWallsResist(
		ExplosionDamageCalculator calculator, Explosion explosion, BlockGetter level, BlockPos pos, BlockState block, FluidState fluid, Operation<Optional<Float>> original
	) {
		if (block.isAir() && SkyCollision.active() && SkyCollision.solidFraction(pos) >= 0.5F) {
			return Optional.of(HOST_RESISTANCE);
		}
		return original.call(calculator, explosion, level, pos, block, fluid);
	}

	@WrapOperation(
		method = "getSeenPercent",
		at = @At(value = "INVOKE", target = "Lnet/minecraft/world/level/Level;clip(Lnet/minecraft/world/level/ClipContext;)Lnet/minecraft/world/phys/BlockHitResult;")
	)
	private static BlockHitResult skycraft$hostWallsShield(Level level, ClipContext context, Operation<BlockHitResult> original) {
		BlockHitResult vanilla = original.call(level, context);
		if (vanilla.getType() != HitResult.Type.MISS || !SkyCollision.active()) {
			return vanilla;
		}
		return SkyClip.refine(level, context.getFrom(), context.getTo(), vanilla, SkyClip.Use.PICK);
	}
}
