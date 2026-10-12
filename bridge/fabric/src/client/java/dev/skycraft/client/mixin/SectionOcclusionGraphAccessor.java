package dev.skycraft.client.mixin;

import java.util.concurrent.atomic.AtomicBoolean;
import net.minecraft.client.renderer.SectionOcclusionGraph;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.gen.Accessor;

/** Portalcraft: force the visible-section list to be rebuilt for the next camera's frustum. */
@Mixin(SectionOcclusionGraph.class)
public interface SectionOcclusionGraphAccessor {
	@Accessor("needsFrustumUpdate")
	AtomicBoolean portalcraft$needsFrustumUpdate();
}
