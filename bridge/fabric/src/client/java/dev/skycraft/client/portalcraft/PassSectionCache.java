package dev.skycraft.client.portalcraft;

import dev.skycraft.SkyCraft;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.concurrent.atomic.AtomicLong;
import net.minecraft.client.Camera;
import net.minecraft.client.Minecraft;
import net.minecraft.client.renderer.DynamicGpuData;
import net.minecraft.client.renderer.LevelRenderer;
import net.minecraft.client.renderer.chunk.ChunkSectionLayer;
import net.minecraft.client.renderer.chunk.SectionRenderDispatcher;
import net.minecraft.world.phys.Vec3;
import org.joml.Quaternionf;

/**
 * Portalcraft (frame-rate experiment 5, bridge #10; off unless -Dportalcraft.passCache=true): every level pass
 * (the primary camera, the second camera, each portal view Minecraft draws) rebuilds its visible-section list
 * (applyFrustum over the section graph, forced per pass because the passes share LevelRenderer's list) and then
 * walks that list again to extract the terrain draw groups (extractSectionDrawGroups, ~28% of the render thread's
 * samples in the facing tunnel). Both only depend on the pass's camera, the graph and the section meshes, so per
 * pass they are kept and reused while the pass's camera has not moved (0.05 blocks, 0.1 deg, same cull window and
 * render distance) and nothing that feeds them changed: an epoch counter is bumped on every section mesh change,
 * section reset or re-position, uber-buffer allocation, removal or resize, graph update and view-area move. Draw
 * groups are only kept once every visible section has finished fading in. A moving camera gets no reuse.
 */
public final class PassSectionCache {
	public static final boolean ENABLED = "true".equals(System.getProperty("portalcraft.passCache"));
	private static final AtomicLong EPOCH = new AtomicLong(1);
	private static final int PASSES = 8;
	private static final double POS_EPS = 0.05, ANG_EPS = Math.toRadians(0.1);

	private static final class Entry {
		long epoch = -1;
		double x, y, z;
		final Quaternionf rot = new Quaternionf();
		final float[] box = new float[4];
		int rd;
		final ArrayList<SectionRenderDispatcher.RenderSection> visible = new ArrayList<>();
		final ArrayList<SectionRenderDispatcher.RenderSection> nearby = new ArrayList<>();
		long groupsEpoch = -1;
		boolean respect;
		@SuppressWarnings("unchecked")
		final List<Object>[] groups = new List[ChunkSectionLayer.values().length];
		final ArrayList<DynamicGpuData.ChunkSectionInfo> infos = new ArrayList<>();
		int largest;
	}

	private static final Entry[] ENTRIES = new Entry[PASSES];
	static {
		for (int i = 0; i < PASSES; i++) {
			ENTRIES[i] = new Entry();
		}
	}
	private static int listPass = -1;  // the pass whose list LevelRenderer's visibleSections holds now (-1 unknown)
	private static long headEpoch, groupsHeadEpoch;
	private static long frustumHits, frustumMisses, groupHits, groupMisses, groupFading, calls;

	private PassSectionCache() {
	}

	/** Anything the lists or draw groups depend on changed (any thread); why: index into REASONS (diagnostics). */
	public static final String[] REASONS = {"all changed", "view area moved", "section mesh", "section reset", "section moved", "allocation added", "allocation removed",
		"buffer resized", "graph update", "graph reset"};
	private static final java.util.concurrent.atomic.AtomicLongArray WHY = new java.util.concurrent.atomic.AtomicLongArray(REASONS.length);

	public static void invalidate(int why) {
		EPOCH.incrementAndGet();
		WHY.incrementAndGet(why);
	}

	/** LevelRenderer.clearVisibleSections: the list no longer belongs to any pass. */
	public static void listCleared() {
		listPass = -1;
	}

	private static int pass() {
		int p = HostViewClient.pass();
		return p >= 0 && p < PASSES ? p : -1;
	}

	private static boolean poseMatches(Entry e, Camera cam, float[] box, int rd) {
		Vec3 pos = cam.position();
		if (Math.abs(pos.x - e.x) > POS_EPS || Math.abs(pos.y - e.y) > POS_EPS || Math.abs(pos.z - e.z) > POS_EPS || rd != e.rd) {
			return false;
		}
		for (int i = 0; i < 4; i++) {
			if (Math.abs(box[i] - e.box[i]) > 0.002F) {
				return false;
			}
		}
		Quaternionf q = new Quaternionf(cam.rotation());
		float dot = Math.abs(q.dot(e.rot));
		return 2.0 * Math.acos(Math.min(1.0F, dot)) <= ANG_EPS;
	}

	/** LevelExtractor.applyFrustum HEAD: true when this pass's cached list was put back (skip the rebuild). */
	public static boolean restoreFrustum(LevelRenderer lr) {
		headEpoch = EPOCH.get();
		int p = pass();
		if (!ENABLED || p < 0) {
			return false;
		}
		Entry e = ENTRIES[p];
		Camera cam = Minecraft.getInstance().gameRenderer.mainCamera();
		if (e.epoch != headEpoch || !poseMatches(e, cam, HostViewClient.cullWindow(), HostViewClient.passRenderDistance())) {
			frustumMisses++;
			return false;
		}
		lr.clearVisibleSections();
		lr.visibleSections().addAll(e.visible);
		lr.nearbyVisibleSections().addAll(e.nearby);
		listPass = p;
		frustumHits++;
		return true;
	}

	/** LevelExtractor.applyFrustum RETURN (a rebuilt list): kept for this pass. */
	public static void storeFrustum(LevelRenderer lr) {
		int p = pass();
		if (!ENABLED || p < 0) {
			return;
		}
		Entry e = ENTRIES[p];
		Camera cam = Minecraft.getInstance().gameRenderer.mainCamera();
		Vec3 pos = cam.position();
		e.x = pos.x;
		e.y = pos.y;
		e.z = pos.z;
		e.rot.set(cam.rotation());
		System.arraycopy(HostViewClient.cullWindow(), 0, e.box, 0, 4);
		e.rd = HostViewClient.passRenderDistance();
		e.visible.clear();
		e.visible.addAll(lr.visibleSections());
		e.nearby.clear();
		e.nearby.addAll(lr.nearbyVisibleSections());
		e.epoch = headEpoch;
		e.groupsEpoch = -1;
		listPass = p;
	}

	/** LevelRenderer.extractSectionDrawGroups HEAD: the cached groups' largest index count, or -1 to extract. */
	@SuppressWarnings({"unchecked", "rawtypes"})
	public static int fillGroups(boolean respect, List<DynamicGpuData.ChunkSectionInfo> infos, Map groups) {
		groupsHeadEpoch = EPOCH.get();
		int p = pass();
		if (!ENABLED || p < 0) {
			return -1;
		}
		log();
		Entry e = ENTRIES[p];
		if (listPass != p || e.epoch != groupsHeadEpoch || e.groupsEpoch != groupsHeadEpoch || e.respect != respect) {
			groupMisses++;
			return -1;
		}
		infos.addAll(e.infos);
		for (ChunkSectionLayer layer : ChunkSectionLayer.values()) {
			((List<Object>) groups.get(layer)).addAll(e.groups[layer.ordinal()]);
		}
		groupHits++;
		return e.largest;
	}

	/** LevelRenderer.extractSectionDrawGroups RETURN (extracted): kept when every section is fully faded in. */
	@SuppressWarnings({"unchecked", "rawtypes"})
	public static void storeGroups(boolean respect, List<DynamicGpuData.ChunkSectionInfo> infos, Map groups, int largest) {
		int p = pass();
		if (!ENABLED || p < 0 || listPass != p) {
			return;
		}
		Entry e = ENTRIES[p];
		if (e.epoch != groupsHeadEpoch) {
			return;
		}
		for (DynamicGpuData.ChunkSectionInfo info : infos) {
			if (info.visibility() < 1.0F) {
				groupFading++;
				return;
			}
		}
		e.infos.clear();
		e.infos.addAll(infos);
		for (ChunkSectionLayer layer : ChunkSectionLayer.values()) {
			List<Object> l = e.groups[layer.ordinal()];
			if (l == null) {
				l = e.groups[layer.ordinal()] = new ArrayList<>();
			}
			l.clear();
			l.addAll((List<Object>) groups.get(layer));
		}
		e.largest = largest;
		e.respect = respect;
		e.groupsEpoch = groupsHeadEpoch;
	}

	private static void log() {
		if (++calls % 3000 != 0) {
			return;
		}
		SkyCraft.LOG.info("Portalcraft: pass section cache: visible lists {} reused / {} rebuilt, draw groups {} reused / {} extracted ({} not kept: fading in), epoch {}", frustumHits,
			frustumMisses, groupHits, groupMisses, groupFading, EPOCH.get());
		StringBuilder b = new StringBuilder();
		for (int i = 0; i < REASONS.length; i++) {
			long n = WHY.getAndSet(i, 0);
			if (n > 0) {
				b.append(REASONS[i]).append(' ').append(n).append(", ");
			}
		}
		SkyCraft.LOG.info("Portalcraft: pass section cache invalidations: {}", b);
		frustumHits = frustumMisses = groupHits = groupMisses = groupFading = 0;
	}
}
