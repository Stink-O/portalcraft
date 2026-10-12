package dev.skycraft.client.mixin;

import com.mojang.blaze3d.pipeline.PipelineCache;
import java.util.concurrent.Executor;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.ModifyArg;

/**
 * Portalcraft: a pipeline first used in the middle of a frame is compiled by PipelineCache.get, which hands the
 * compile to the shared background pool and then blocks the render thread on it (join). When that pool is busy
 * with section rebuilds that cannot finish without the render thread (their uploads wait in the staging buffer
 * only the render thread drains: a burst of sections after a teleport at render distance 24), the compile
 * never runs: the hidden Minecraft stopped rendering mid-run, never quit with its host and ignored SIGTERM
 * (2026-10-01 19:42 and 20:27; jstack in cache/bridge/shots/edge7/jstack_mc.txt: render thread parked in
 * PipelineCache.get, every Worker-Main thread busy in SectionRenderDispatcher). The render thread waits for the
 * result anyway, so the compile runs on it directly: the same work, no dependence on the pool.
 */
@Mixin(PipelineCache.class)
public abstract class PipelineCacheInlineMixin {
	@ModifyArg(method = "get", at = @At(value = "INVOKE", target = "Lcom/mojang/renderpearl/api/device/GpuDevice;compilePipeline(Lcom/mojang/renderpearl/api/pipeline/RenderPipeline;Lcom/mojang/renderpearl/api/pipeline/ShaderSource;Ljava/util/concurrent/Executor;)Ljava/util/concurrent/CompletableFuture;"), index = 2)
	private Executor portalcraft$inline(Executor pool) {
		return Runnable::run;
	}
}
