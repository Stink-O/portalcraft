package dev.skycraft.client.mixin;

import com.mojang.blaze3d.resource.CrossFrameResourcePool;
import net.minecraft.client.DeltaTracker;
import net.minecraft.client.renderer.GameRenderer;
import net.minecraft.client.renderer.GlobalSettingsUniform;
import net.minecraft.client.renderer.fog.FogRenderer;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.gen.Accessor;
import org.spongepowered.asm.mixin.gen.Invoker;

/** Portalcraft: what the second camera's level pass needs from GameRenderer (GameRendererViewMixin). */
@Mixin(GameRenderer.class)
public interface GameRendererAccessor {
	@Accessor("fogRenderer")
	FogRenderer portalcraft$fogRenderer();

	@Accessor("resourcePool")
	CrossFrameResourcePool portalcraft$resourcePool();

	@Accessor("globalSettingsUniform")
	GlobalSettingsUniform portalcraft$globalSettings();

	@Invoker("extractCamera")
	void portalcraft$extractCamera(DeltaTracker deltaTracker, float worldPartialTicks);
}
