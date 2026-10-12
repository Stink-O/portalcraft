package dev.skycraft.world;

import net.minecraft.core.BlockPos;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.entity.player.Player;
import net.minecraft.world.phys.AABB;
import net.minecraft.world.phys.shapes.BooleanOp;
import net.minecraft.world.phys.shapes.Shapes;
import net.minecraft.world.phys.shapes.VoxelShape;
import org.jspecify.annotations.Nullable;

/**
 * Portalcraft (bridge #48): how Portal 2's chamber geometry (the host's voxel field, SkyCollision) and the portals' holes enter
 * Minecraft's shape queries. Portal 2's walls are not blocks: they are shapes merged into a cell's collision. Two doors use the same
 * rules, so an entity meets the same world whether it moves (BlockCollisionsMixin) or flies (ClipContextMixin: every block clip with a collider
 * context, i.e. arrows, thrown things, fireballs, line of sight): the cell's own shape, plus the host's, with the holes of the open
 * portals cut out for whoever can enter them.
 */
public final class HostShapes {
	private HostShapes() {
	}

	/**
	 * The shape with every open portal's hole cut out, for this entity: the player's holes are the host's (PortalHoles), every other entity's are
	 * the linked pairs' (PortalPair; mobs, items and projectiles are carried through by EntityTransit). {@code reach} deepens the latter for a
	 * thing that travels that far in a tick, so a fast arrow gets through the opening instead of sticking in the wall behind it.
	 * No entity or no hole there: the shape itself.
	 */
	public static VoxelShape carve(@Nullable Entity entity, BlockPos pos, VoxelShape shape, double reach) {
		if (entity == null || shape.isEmpty()) {
			return shape;
		}
		if (entity instanceof Player) {
			VoxelShape carved = PortalHoles.carve(pos, shape);
			return carved != null ? carved : shape;
		}
		if (!PortalPair.anyLinked()) {
			return shape;
		}
		for (int i = 0; i < PortalPair.SLOTS; i++) {
			AABB h = reach > 2.0 ? PortalPair.hole(i, reach) : PortalPair.hole(i);
			if (h == null || !h.intersects(pos.getX(), pos.getY(), pos.getZ(), pos.getX() + 1, pos.getY() + 1, pos.getZ() + 1)) {
				continue;
			}
			VoxelShape hole = Shapes.create(h.move(-pos.getX(), -pos.getY(), -pos.getZ()).intersect(new AABB(0, 0, 0, 1, 1, 1)));
			shape = Shapes.join(shape, hole, BooleanOp.ONLY_FIRST);
		}
		return shape;
	}

	/**
	 * (bridge #72) The holes this entity can enter, as world boxes (the same ones {@link #carve} cuts out of blocks and host voxels), or null when none is
	 * open. Movers (doors, panels, the invisible catch plates under a pit: SkyMovers) are cut by them too: a floor portal over a thin mover slab
	 * 16 units under it held the Minecraft player 0.4 block under the plane, its eye never crossing.
	 */
	public static AABB @Nullable [] holesFor(@Nullable Entity entity) {
		if (entity == null) {
			return null;
		}
		if (entity instanceof Player) {
			return PortalHoles.snapshot();
		}
		if (!PortalPair.anyLinked()) {
			return null;
		}
		AABB[] out = null;
		int n = 0;
		for (int i = 0; i < PortalPair.SLOTS; i++) {
			AABB h = PortalPair.hole(i);
			if (h != null) {
				if (out == null) {
					out = new AABB[PortalPair.SLOTS];
				}
				out[n++] = h;
			}
		}
		return out == null ? null : java.util.Arrays.copyOf(out, n);
	}

	/**
	 * A collider clip's shape for one cell: the cell's own shape and the host's, holes cut. The host's voxels count wherever the host has
	 * geometry, whatever the cell holds (a placed block sunk into a chamber floor, a revealed dig block, air).
	 */
	public static VoxelShape clipShape(@Nullable Entity entity, BlockPos pos, VoxelShape own, double reach) {
		own = carve(entity, pos, own, reach);
		VoxelShape host = SkyCollision.shapeAt(pos);
		if (host == null) {
			return own;
		}
		host = carve(entity, pos, host, reach);
		return own.isEmpty() ? host : Shapes.or(own, host);
	}
}
