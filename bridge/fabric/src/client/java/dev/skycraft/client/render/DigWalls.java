package dev.skycraft.client.render;

import dev.skycraft.world.ChamberBlocks;
import dev.skycraft.world.DigHost;
import dev.skycraft.world.DigMath;
import dev.skycraft.world.DigWallQuads;
import dev.skycraft.world.SkyCollision;
import dev.skycraft.world.SkyDig;
import net.minecraft.client.multiplayer.ClientLevel;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Direction;
import net.minecraft.util.LightCoordsUtil;
import net.minecraft.world.level.LightLayer;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.block.state.BlockState;

/**
 * The walls of holes dug into Portal 2's chambers. Adapted from SkyCraft's DigWalls (MIT, chasmlol/SkyCraft; see
 * LICENSE-SkyCraft): a cell only partly inside the host's solid stays the host's; where it borders a dug cell, the part of it under
 * the surface is drawn as Minecraft faces of ordinary blocks (SkyCraft: dirt, stone), emitted as extra quads in the section mesh, so
 * a hole is closed all round and nothing sticks up out of the surface. Here the host solid is the voxel field
 * (SkyCollision.rawBits, 8x8x8 sub-voxels per block) instead of triangles: for every dug cell D and every neighbour N across a face of D
 * that is not dug and has no occluding block, one quad per merged run of solid sub-voxel faces of N's boundary layer next to D, facing
 * into D. A wholly solid N is a single full-face quad.
 *
 * Which texture (bridge #38): the block N would be if it were revealed ({@link DigMath#kindAtDepth}): a cell only partly inside the
 * host's geometry is the surface layer, the chamber's own block family (the family of the chamber block next to D, else the surface last
 * dug); a cell deeper in rock is concrete or stone. Lit with Minecraft's own light at the dug cell (block and sky), like any block face.
 *
 * The pure part (no Minecraft state; unit tested) is {@link DigWallQuads}; this class feeds it the client's level.
 */
final class DigWalls {
	private DigWalls() {
	}

	interface Out {
		/** One quad: four corners (x, y, z, section-relative) and their atlas UVs, facing {@code normal}. */
		void quad(float[] xyz, float[] uv, int light, Direction normal);
	}

	/** Adds the walls around the dug cells of one section of the client's level (bits: x + 16z + 256y). Returns the number of quads. */
	static int add(ClientLevel level, int sx, int sy, int sz, long[] dug, int world, java.util.function.Function<BlockState, float[]> faces, Out out) {
		Direction[] dirs = Direction.values();
		return DigWallQuads.collect(new LevelSource(level, world), sx, sy, sz, dug, kind -> faces.apply(stateOf(kind)), (xyz, uv, light, normal) -> out.quad(xyz, uv, light, dirs[normal]));
	}

	/** The block whose texture a reveal kind is drawn with (the client has no seed: plain stone for the ores). */
	static BlockState stateOf(int kind) {
		return kind == DigMath.BK_STONE ? Blocks.STONE.defaultBlockState() : kind == DigMath.BK_BEDROCK ? Blocks.BEDROCK.defaultBlockState() : ChamberBlocks.stateFor(kind);
	}

	/** {@link Source} on the client's level: Minecraft's blocks and light, the dug set, the host's voxels, the reveal rule's depth. */
	private static final class LevelSource implements DigWallQuads.Source, DigMath.Field {
		final ClientLevel level;
		final int world;
		final BlockPos.MutableBlockPos pos = new BlockPos.MutableBlockPos();
		final BlockPos.MutableBlockPos near = new BlockPos.MutableBlockPos();

		LevelSource(ClientLevel level, int world) {
			this.level = level;
			this.world = world;
		}

		@Override
		public boolean dug(int x, int y, int z) {
			return SkyDig.isDug(this.world, this.pos.set(x, y, z));
		}

		@Override
		public boolean occludes(int x, int y, int z) {
			return this.level.getBlockState(this.pos.set(x, y, z)).canOcclude();
		}

		@Override
		public long[] bits(int x, int y, int z) {
			this.pos.set(x, y, z);
			// (bridge #46) a cell holding only invisible collision (clip, invisible walls) has no wall to draw
			return SkyCollision.rawGhostOnly(this.pos) ? null : SkyCollision.rawBits(this.pos);
		}

		@Override
		public int light(int x, int y, int z) {
			this.pos.set(x, y, z);
			return LightCoordsUtil.pack(this.level.getBrightness(LightLayer.BLOCK, this.pos), this.level.getBrightness(LightLayer.SKY, this.pos));
		}

		@Override
		public int kind(int x, int y, int z, int nx, int ny, int nz) {
			int family = family(x, y, z);
			int n = samples(nx, ny, nz);
			// partly inside the host's geometry: the surface layer. Wholly inside (not revealed yet, or waiting for its region): by depth.
			return DigMath.kindAtDepth(family, n >= DigMath.WHOLE ? DigMath.depth(this, nx, ny, nz) : 0);
		}

		/** The surface family of a dug cell: a chamber surface block next to it, else the one last dug (what the crosshair said). */
		private int family(int x, int y, int z) {
			for (Direction d : Direction.values()) {
				int m = ChamberBlocks.materialOf(this.level.getBlockState(this.near.set(x + d.getStepX(), y + d.getStepY(), z + d.getStepZ())));
				if (m > 0 && m != DigMath.CH_CONCRETE) {
					return m;
				}
			}
			return DigMath.materialOf(DigHost.material());
		}

		// DigMath.Field (for the depth of a wholly solid wall cell)
		@Override
		public int samples(int x, int y, int z) {
			return SkyCollision.visibleSamples(this.pos.set(x, y, z));
		}

		@Override
		public boolean limit(int x, int y, int z) {
			return SkyCollision.rawLimit(this.pos.set(x, y, z));
		}
	}
}
