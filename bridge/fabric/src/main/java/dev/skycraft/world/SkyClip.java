package dev.skycraft.world;

import java.util.ArrayList;
import java.util.List;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Direction;
import net.minecraft.world.phys.AABB;
import net.minecraft.world.phys.BlockHitResult;
import net.minecraft.world.phys.HitResult;
import net.minecraft.world.phys.Vec3;

/**
 * Makes Minecraft ray casts (arrows and other projectiles, the crosshair pick) hit Skyrim's exact
 * collision triangles. Vanilla still clips against real Minecraft blocks; whichever is nearer wins.
 */
public final class SkyClip {
	private SkyClip() {
	}

	public enum Use {
		/** A projectile: the hit cell is the one the surface is in (it sticks there). */
		PROJECTILE,
		/** The player's crosshair: the hit cell is where a block placed against the surface goes. */
		PICK
	}

	private static final ThreadLocal<List<SkyTri>> SCRATCH = ThreadLocal.withInitial(ArrayList::new);

	public static BlockHitResult refine(Vec3 from, Vec3 to, BlockHitResult vanilla, Use use) {
		return refine(null, from, to, vanilla, use);
	}

	/**
	 * Portalcraft: with a level, the crosshair (PICK) also meets the host's voxel collision (the 1/8-block
	 * CollisionField shapes of Portal 2's chambers, SkyCollision.shapeAt): the voxel variant of SkyClip, used
	 * when no exact triangles are there (the host exports voxels only).
	 */
	public static BlockHitResult refine(net.minecraft.world.level.@org.jspecify.annotations.Nullable BlockGetter level, Vec3 from, Vec3 to, BlockHitResult vanilla, Use use) {
		BlockHitResult base = refineHost(level, from, to, vanilla, use);
		if (use != Use.PICK) {
			return base;
		}
		// Portalcraft (bridge #43): the crosshair also meets the host's movers (doors, panels, lifts...) at their poses; the nearest hit wins
		SkyMovers.Hit mh = SkyMovers.pick(from, to);
		if (mh == null) {
			return base;
		}
		Vec3 location = new Vec3(mh.x(), mh.y(), mh.z());
		if (base.getType() != HitResult.Type.MISS && from.distanceToSqr(base.getLocation()) <= from.distanceToSqr(location)) {
			return base;
		}
		if (!mh.minable()) {
			// Bridge #60: a physics prop / NPC (cube, turret, radio, core) is hit with attacks, not dug. Its Minecraft stand-in entity (SkyrimActorEntity)
			// has about the prop's own box, and vanilla's pick lets an entity win only when it is strictly nearer than the block hit: the mover's shape
			// (same surface) won the tie, hitResult was a block hit, and the attack never reached the stand-in (no push, no damage). Report the hit a
			// little in front of the surface so the entity is the nearer one; with no stand-in the hit is still this mover, which blocks the ray.
			double len = from.distanceTo(location);
			if (len > 1.0E-6) {
				double back = Math.min(0.75, len);
				location = location.add(from.subtract(location).scale(back / len));
			}
		}
		SkyRay.Hit hit = new SkyRay.Hit(mh.t(), mh.x(), mh.y(), mh.z(), mh.nx(), mh.ny(), mh.nz());
		Direction face = Direction.values()[SkyRay.dominantFace(mh.nx(), mh.ny(), mh.nz())];
		int[] cell = SkyRay.placementCell(hit);
		int[] surface = SkyRay.surfaceCell(hit);
		return new MoverHitResult(location, face, new BlockPos(cell[0], cell[1], cell[2]), mh, new BlockPos(surface[0], surface[1], surface[2]));
	}

	/**
	 * Portalcraft (bridge #48): what a flying thing (an arrow, a thrown snowball, a reach check) meets: vanilla's clip (which, with ClipContextMixin, already
	 * holds the host's static voxels), Skyrim's exact triangles, and the host's movers (doors, panels, lifts) at their poses. The nearest wins. A hit on a
	 * mover points at the cell the surface is in, like any other projectile hit.
	 */
	public static BlockHitResult refineProjectile(Vec3 from, Vec3 to, BlockHitResult vanilla) {
		BlockHitResult base = refine(null, from, to, vanilla, Use.PROJECTILE);
		SkyMovers.Hit mh = SkyMovers.pick(from, to);
		if (mh == null) {
			return base;
		}
		Vec3 location = new Vec3(mh.x(), mh.y(), mh.z());
		if (base.getType() != HitResult.Type.MISS && from.distanceToSqr(base.getLocation()) <= from.distanceToSqr(location)) {
			return base;
		}
		SkyRay.Hit hit = new SkyRay.Hit(mh.t(), mh.x(), mh.y(), mh.z(), mh.nx(), mh.ny(), mh.nz());
		Direction face = Direction.values()[SkyRay.dominantFace(mh.nx(), mh.ny(), mh.nz())];
		int[] surface = SkyRay.surfaceCell(hit);
		BlockPos cell = new BlockPos(surface[0], surface[1], surface[2]);
		return new MoverHitResult(location, face, cell, mh, cell);
	}

	private static BlockHitResult refineHost(net.minecraft.world.level.@org.jspecify.annotations.Nullable BlockGetter level, Vec3 from, Vec3 to, BlockHitResult vanilla, Use use) {
		SkyRay.Hit hit = cast(from, to);
		if (hit == null && level != null) {  // (bridge #20: any use given a level; ItemMixin's buckets as PROJECTILE)
			hit = castVoxels(level, from, to);
		}
		if (hit == null) {
			return vanilla;
		}
		Vec3 location = new Vec3(hit.x(), hit.y(), hit.z());
		if (vanilla.getType() != HitResult.Type.MISS && from.distanceToSqr(vanilla.getLocation()) <= from.distanceToSqr(location)) {
			return vanilla;
		}
		Direction face = Direction.values()[SkyRay.dominantFace(hit.nx(), hit.ny(), hit.nz())];
		int[] cell = use == Use.PICK ? SkyRay.placementCell(hit) : SkyRay.surfaceCell(hit);
		int[] surface = SkyRay.surfaceCell(hit);
		// Portalcraft (bridge #23): a host floor whose top is in the lower half of its cell is replaceable, like a snow layer:
		// a block placed on it goes into that cell (sunk into the floor by the sliver, hidden inside it), never floating on top
		if (use == Use.PICK && hit.ny() > 0.7) {
			double f = hit.y() - surface[1];
			cell = new int[] {surface[0], f <= 0.5 ? surface[1] : surface[1] + 1, surface[2]};
		}
		return new SkyrimHitResult(location, face, new BlockPos(cell[0], cell[1], cell[2]), hit.nx(), hit.ny(), hit.nz(), new BlockPos(surface[0], surface[1], surface[2]));
	}

	/** Nearest Skyrim triangle hit on the segment, or null. */
	public static SkyRay.Hit cast(Vec3 from, Vec3 to) {
		List<SkyTri> tris = SCRATCH.get();
		tris.clear();
		SkyCollision.trianglesNear(new AABB(from, to).inflate(0.01), tris);
		if (tris.isEmpty()) {
			return null;
		}
		SkyRay.Hit hit = SkyRay.cast(tris, from.x, from.y, from.z, to.x, to.y, to.z);
		tris.clear();
		return hit;
	}

	/**
	 * Portalcraft: the nearest host collision voxel on the segment, or null. Cells holding a real Minecraft block
	 * are left to vanilla's clip (a placed block sunk into a Portal 2 floor is targeted as the block).
	 */
	public static SkyRay.@org.jspecify.annotations.Nullable Hit castVoxels(net.minecraft.world.level.BlockGetter level, Vec3 from, Vec3 to) {
		if (!SkyCollision.active() || from.distanceToSqr(to) < 1e-12) {
			return null;
		}
		BlockHitResult r = net.minecraft.world.level.BlockGetter.traverseBlocks(from, to, level, (getter, pos) -> {
			net.minecraft.world.phys.shapes.VoxelShape shape = SkyCollision.shapeAt(pos);
			if (shape == null || !getter.getBlockState(pos).isAir() || SkyCollision.rawGhostOnly(pos)) {
				return null;  // (bridge #46: a cell with only invisible collision (clip, invisible walls) is not a surface: the crosshair looks past it)
			}
			BlockHitResult h = shape.clip(from, to, pos);
			return h != null && !h.isInside() ? h : null;  // (an eye inside a voxel: look past it)
		}, getter -> null);
		if (r == null || r.getType() == HitResult.Type.MISS) {
			return null;
		}
		Vec3 loc = r.getLocation();
		double t = Math.sqrt(from.distanceToSqr(loc) / from.distanceToSqr(to));
		var n = r.getDirection().getUnitVec3();
		return new SkyRay.Hit(t, loc.x, loc.y, loc.z, n.x, n.y, n.z);
	}

	/** (bridge #43) A hit on a host mover (a door, a panel, a lift...): the mover and the dig lattice cell holding the hit voxel. */
	public static final class MoverHitResult extends SkyrimHitResult {
		public final SkyMovers.Hit mover;

		public MoverHitResult(Vec3 location, Direction direction, BlockPos pos, SkyMovers.Hit mover, BlockPos surface) {
			super(location, direction, pos, mover.nx(), mover.ny(), mover.nz(), surface);
			this.mover = mover;
		}
	}

	/** A hit on Skyrim geometry (not a Minecraft block). Keeps the exact surface normal. */
	public static class SkyrimHitResult extends BlockHitResult {
		public final double nx, ny, nz;
		/** Portalcraft (bridge #23): the host cell the surface is in (the outline is its voxel shape). */
		public final BlockPos surface;

		public SkyrimHitResult(Vec3 location, Direction direction, BlockPos pos, double nx, double ny, double nz) {
			this(location, direction, pos, nx, ny, nz, pos);
		}

		public SkyrimHitResult(Vec3 location, Direction direction, BlockPos pos, double nx, double ny, double nz, BlockPos surface) {
			super(location, direction, pos, false);
			this.nx = nx;
			this.ny = ny;
			this.nz = nz;
			this.surface = surface;
		}
	}
}
