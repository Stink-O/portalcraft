package dev.skycraft.world;

import net.minecraft.core.BlockPos;
import net.minecraft.world.phys.AABB;
import net.minecraft.world.phys.shapes.BooleanOp;
import net.minecraft.world.phys.shapes.Shapes;
import net.minecraft.world.phys.shapes.VoxelShape;
import org.jetbrains.annotations.Nullable;

/**
 * Portalcraft (bridge #13): the open portals' hole volumes, sent by the Portal 2 host while its Minecraft player
 * leads. A player's block collision has them carved out, so it can walk or fall into a portal that sits on
 * Minecraft blocks (the host's Chell gets the same carve through its block hook); Portal 2 then carries the body
 * through and the host teleports this player to the exit.
 */
public final class PortalHoles {
	private static final double[][] MIN = new double[PortalPair.SLOTS][3];
	private static final AABB[] LIVE = new AABB[PortalPair.SLOTS];  // by portal slot (bridge #32: every active pair; slot = group * 2 + colour)
	private static int logs;

	private PortalHoles() {
	}

	public static synchronized void set(int code, double x, double y, double z) {
		int group = (code >> 8) & 0xFF;
		if (group >= PortalPair.SLOTS / 2) {
			return;
		}
		int i = (code & 1) | (group << 1);
		if ((code & 4) != 0) {
			LIVE[i] = null;
		} else if ((code & 2) == 0) {
			MIN[i][0] = x;
			MIN[i][1] = y;
			MIN[i][2] = z;
		} else {
			LIVE[i] = new AABB(MIN[i][0], MIN[i][1], MIN[i][2], x, y, z);
		}
		if (logs++ < 20) {
			dev.skycraft.SkyCraft.LOG.info("Portalcraft: portal hole {} {}", i, LIVE[i]);
		}
	}

	/** (bridge #72) Every open hole now (a copy), or null when there is none: what movers (SkyMovers) are cut by for the player, as blocks are by {@link #carve}. */
	public static synchronized AABB @Nullable [] snapshot() {
		int n = 0;
		for (AABB h : LIVE) {
			n += h != null ? 1 : 0;
		}
		if (n == 0) {
			return null;
		}
		AABB[] out = new AABB[n];
		n = 0;
		for (AABB h : LIVE) {
			if (h != null) {
				out[n++] = h;
			}
		}
		return out;
	}

	/** Portalcraft (bridge #20): this point is inside an open portal's hole (the player's eye on its way through). */
	public static synchronized boolean contains(double x, double y, double z) {
		return intersects(new AABB(x, y, z, x, y, z));
	}

	/** Portalcraft (bridge #20): this box touches an open portal's hole. */
	public static synchronized boolean intersects(AABB box) {
		for (AABB h : LIVE) {
			if (h != null && h.intersects(box.inflate(1e-4))) {
				return true;
			}
		}
		return false;
	}

	/** The block's collision shape with the holes cut out, or null when no hole touches it. */
	public static synchronized @Nullable VoxelShape carve(BlockPos pos, VoxelShape shape) {
		if (shape.isEmpty()) {
			return null;
		}
		VoxelShape out = null;
		for (AABB h : LIVE) {
			if (h == null || !h.intersects(pos.getX(), pos.getY(), pos.getZ(), pos.getX() + 1, pos.getY() + 1, pos.getZ() + 1)) {
				continue;
			}
			VoxelShape hole = Shapes.create(h.move(-pos.getX(), -pos.getY(), -pos.getZ()).intersect(new AABB(0, 0, 0, 1, 1, 1)));
			out = Shapes.join(out == null ? shape : out, hole, BooleanOp.ONLY_FIRST);
		}
		return out;
	}
}
