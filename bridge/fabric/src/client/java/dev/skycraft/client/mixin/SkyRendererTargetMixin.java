package dev.skycraft.client.mixin;

import com.mojang.blaze3d.pipeline.RenderTarget;
import net.minecraft.client.Minecraft;
import net.minecraft.client.renderer.SkyRenderer;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Redirect;

/**
 * Portalcraft: the sky renderer keeps the main target it was created with; it draws into the
 * current one instead (a portal pass's own target, see GameRendererTargetMixin).
 */
@Mixin(SkyRenderer.class)
public abstract class SkyRendererTargetMixin {
	@Redirect(
		method = "render",
		at = @At(value = "FIELD", target = "Lnet/minecraft/client/renderer/SkyRenderer;renderTarget:Lcom/mojang/blaze3d/pipeline/RenderTarget;", opcode = org.objectweb.asm.Opcodes.GETFIELD)
	)
	private RenderTarget portalcraft$currentTarget(SkyRenderer self) {
		return Minecraft.getInstance().gameRenderer.mainRenderTarget();
	}
}
