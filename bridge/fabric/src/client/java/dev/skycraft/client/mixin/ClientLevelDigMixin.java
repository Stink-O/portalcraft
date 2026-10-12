package dev.skycraft.client.mixin;

import dev.skycraft.client.SkyDigClient;
import net.minecraft.client.multiplayer.ClientLevel;
import net.minecraft.core.BlockPos;
import net.minecraft.world.level.block.state.BlockState;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/**
 * A block of a dug cell breaking reveals what is around it (SkyDigClient). Adapted from SkyCraft's ClientLevelDigMixin (MIT,
 * chasmlol/SkyCraft; see LICENSE-SkyCraft); same Minecraft version, same injection point.
 */
@Mixin(ClientLevel.class)
public abstract class ClientLevelDigMixin {
	@Inject(method = "setBlocksDirty", at = @At("HEAD"))
	private void skycraft$blockChanged(BlockPos pos, BlockState oldState, BlockState newState, CallbackInfo ci) {
		SkyDigClient.blockChanged((ClientLevel) (Object) this, pos, oldState, newState);
	}
}
