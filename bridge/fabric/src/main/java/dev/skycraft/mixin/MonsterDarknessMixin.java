package dev.skycraft.mixin;

import dev.skycraft.portalcraft.HostWorld;
import net.minecraft.core.BlockPos;
import net.minecraft.util.RandomSource;
import net.minecraft.world.entity.monster.Monster;
import net.minecraft.world.level.LightLayer;
import net.minecraft.world.level.ServerLevelAccessor;
import net.minecraft.world.level.dimension.DimensionType;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * Portalcraft (bridge #19): under Portal 2's roof (a cell the host sent its light for: above a block placed in a
 * chamber) Minecraft's sky light does not reach; it is dark enough for a monster when Portal 2's light there and
 * Minecraft's block light (torches) both are, by the dimension's own monster light test.
 */
@Mixin(Monster.class)
public abstract class MonsterDarknessMixin {
	private static int portalcraft$logs;
	@Inject(method = "isDarkEnoughToSpawn", at = @At("HEAD"), cancellable = true)
	private static void portalcraft$hostDark(ServerLevelAccessor level, BlockPos pos, RandomSource random, CallbackInfoReturnable<Boolean> cir) {
		int host = HostWorld.hostLightAt(pos);
		if (host < 0) {
			return;
		}
		DimensionType dimensionType = level.dimensionType();
		int block = level.getBrightness(LightLayer.BLOCK, pos);
		int limit = dimensionType.monsterSpawnBlockLightLimit();
		if (limit < 15 && block > limit) {
			cir.setReturnValue(false);
			return;
		}
		boolean dark = Math.max(host, block) <= dimensionType.monsterSpawnLightTest().sample(random);
		if (dark && portalcraft$logs++ < 30) {
			dev.skycraft.SkyCraft.LOG.info("Portalcraft: dark enough to spawn under Portal 2's roof at {} (Portal 2 light {}, block light {})", pos, host, block);
		}
		cir.setReturnValue(dark);
	}
}
