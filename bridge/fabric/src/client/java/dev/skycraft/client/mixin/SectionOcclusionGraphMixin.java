package dev.skycraft.client.mixin;

import dev.skycraft.client.portalcraft.HostViewClient;
import it.unimi.dsi.fastutil.longs.LongOpenHashSet;
import java.util.concurrent.Future;
import net.minecraft.client.Minecraft;
import net.minecraft.world.level.ChunkPos;
import net.minecraft.client.renderer.SectionOcclusionGraph;
import net.minecraft.client.renderer.state.level.CameraRenderState;
import net.minecraft.client.renderer.state.level.ChunkLoadingRenderState;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/**
 * Portalcraft: after the host's camera jumps (a portal transit, or the switch between the main and
 * the portal view stream), Minecraft recomputes its visible-section graph on a background thread and
 * renders from the old graph until that finishes, so the frames shipped to the host showed white
 * blocks where sections were missing. On such a jump the frame waits for the new graph.
 */
@Mixin(SectionOcclusionGraph.class)
public abstract class SectionOcclusionGraphMixin {
	@Shadow private Future<?> fullUpdateTask;
	@Shadow @org.spongepowered.asm.mixin.Final private LongOpenHashSet loadedChunks;

	/**
	 * (bridge #49) The graph learns which chunks are loaded from the client chunk cache's per-frame "added" sets, which the level extractor hands over (and flips
	 * away) in EVERY pass it extracts, while the update that reads them is cancelled in the second camera's and the portal passes (above). A chunk that was
	 * loaded and announced in such a frame is never known to the graph: a camera in it (or a whole region the camera jumped to, like the Minecraft stage seen
	 * through a portal, 700 blocks up) waits for that chunk for ever and nothing is drawn. When the graph is waiting for chunks the cache holds, tell it.
	 */
	@Inject(method = "update", at = @At("HEAD"))
	private void portalcraft$knowLoadedChunks(CameraRenderState camera, int fov, ChunkLoadingRenderState chunkLoadingRenderState, CallbackInfo ci) {
		if (HostViewClient.pass() != 0 || !HostViewClient.active()) {
			return;
		}
		SectionOcclusionGraph self = (SectionOcclusionGraph) (Object) this;
		if (self.expectedChunks().isEmpty()) {
			return;
		}
		Minecraft minecraft = Minecraft.getInstance();
		if (minecraft.level == null) {
			return;
		}
		var cache = minecraft.level.getChunkSource();
		int cx = (int) Math.floor(camera.pos.x) >> 4, cz = (int) Math.floor(camera.pos.z) >> 4;
		int r = minecraft.options.getEffectiveRenderDistance() + 1;
		for (int x = cx - r; x <= cx + r; x++) {
			for (int z = cz - r; z <= cz + r; z++) {
				if (cache.hasChunk(x, z)) {
					this.loadedChunks.add(ChunkPos.pack(x, z));
				}
			}
		}
	}


	/** The second camera's pass uses the primary camera's graph (smart culling is off in host views). */
	@Inject(method = "update", at = @At("HEAD"), cancellable = true)
	private void portalcraft$secondPassKeepsGraph(CameraRenderState camera, int fov, ChunkLoadingRenderState chunkLoadingRenderState, CallbackInfo ci) {
		if (HostViewClient.pass() != 0) {
			ci.cancel();
		}
	}

	@Inject(method = "update", at = @At("TAIL"))
	private void portalcraft$waitAfterJump(CameraRenderState camera, int fov, ChunkLoadingRenderState chunkLoadingRenderState, CallbackInfo ci) {
		if (!HostViewClient.consumeJump()) {
			return;
		}
		Future<?> task = this.fullUpdateTask;
		if (task != null) {
			try {
				task.get(50, java.util.concurrent.TimeUnit.MILLISECONDS);
			} catch (Exception ignored) {
				// a slow graph update: render anyway rather than stall Minecraft
			}
		}
	}
}
