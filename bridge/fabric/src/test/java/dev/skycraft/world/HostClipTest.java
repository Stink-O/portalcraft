package dev.skycraft.world;

import static org.junit.jupiter.api.Assertions.*;

import net.minecraft.core.BlockPos;
import net.minecraft.core.Direction;
import net.minecraft.world.phys.AABB;
import net.minecraft.world.phys.BlockHitResult;
import net.minecraft.world.phys.HitResult;
import net.minecraft.world.phys.Vec3;
import net.minecraft.world.phys.shapes.Shapes;
import org.junit.jupiter.api.AfterEach;
import org.junit.jupiter.api.Test;

/** Bridge #48: flying things meet the host's movers and stay stuck in them; the shape rules without an entity. */
class HostClipTest {
	@AfterEach
	void clean() {
		SkyMovers.clear();
	}

	private static final float[] LATTICE = {0, 0, 0, 0};

	@Test
	void anArrowHitsAMoverAndNamesTheCellInsideTheSurface() {
		SkyMovers.setForTest(5, 7, new AABB[] {new AABB(0, 0, 0, 1, 2, 0.1)}, 10, 0, 0, 0, 0, 0, 1, LATTICE);
		Vec3 from = new Vec3(10.5, 1.5, 5), to = new Vec3(10.5, 1.5, -5);
		BlockHitResult miss = BlockHitResult.miss(to, Direction.NORTH, BlockPos.containing(to));
		BlockHitResult hit = SkyClip.refineProjectile(from, to, miss);
		assertEquals(HitResult.Type.BLOCK, hit.getType());
		assertTrue(hit instanceof SkyClip.MoverHitResult);
		assertEquals(5, ((SkyClip.MoverHitResult) hit).mover.moverId());
		assertEquals(0.1, hit.getLocation().z, 1e-9);
		assertEquals(new BlockPos(10, 1, 0), hit.getBlockPos());
	}

	@Test
	void aNearerVanillaHitWins() {
		SkyMovers.setForTest(5, 7, new AABB[] {new AABB(0, 0, 0, 1, 2, 0.1)}, 10, 0, 0, 0, 0, 0, 1, LATTICE);
		Vec3 from = new Vec3(10.5, 1.5, 5), to = new Vec3(10.5, 1.5, -5);
		BlockHitResult wall = new BlockHitResult(new Vec3(10.5, 1.5, 2.0), Direction.SOUTH, new BlockPos(10, 1, 1), false);
		assertSame(wall, SkyClip.refineProjectile(from, to, wall));
		BlockHitResult far = new BlockHitResult(new Vec3(10.5, 1.5, -3.0), Direction.SOUTH, new BlockPos(10, 1, -3), false);
		assertTrue(SkyClip.refineProjectile(from, to, far) instanceof SkyClip.MoverHitResult);
	}

	@Test
	void noMoverNoChange() {
		Vec3 from = new Vec3(0, 0, 5), to = new Vec3(0, 0, -5);
		BlockHitResult miss = BlockHitResult.miss(to, Direction.NORTH, BlockPos.containing(to));
		assertSame(miss, SkyClip.refineProjectile(from, to, miss));
	}

	@Test
	void aStuckThingGoesWhereItsMoverGoes() {
		SkyMovers.setForTest(5, 7, new AABB[] {new AABB(0, 0, 0, 1, 2, 0.1)}, 10, 0, 0, 0, 0, 0, 1, LATTICE);
		SkyMovers.Mount m = SkyMovers.mount(5, new Vec3(10.5, 1.5, 0.15), new Vec3(0, 0, -1));
		assertNotNull(m);
		// the door slides 3 blocks along z
		SkyMovers.setForTest(5, 7, new AABB[] {new AABB(0, 0, 0, 1, 2, 0.1)}, 10, 0, 3, 0, 0, 0, 1, LATTICE);
		SkyMovers.Placed p = SkyMovers.place(m);
		assertNotNull(p);
		assertEquals(10.5, p.pos().x, 1e-9);
		assertEquals(3.15, p.pos().z, 1e-9);
		assertEquals(-1.0, p.dir().z, 1e-9);
		// it turns 90 degrees about up: the arrow's direction turns with it
		double s = Math.sqrt(0.5);
		SkyMovers.setForTest(5, 7, new AABB[] {new AABB(0, 0, 0, 1, 2, 0.1)}, 10, 0, 0, 0, s, 0, s, LATTICE);
		p = SkyMovers.place(m);
		assertNotNull(p);
		assertEquals(0.0, p.dir().z, 1e-6);
		assertEquals(1.0, Math.abs(p.dir().x), 1e-6);
		// the mover is gone: nothing holds it
		SkyMovers.clear();
		assertNull(SkyMovers.place(m));
	}

	@Test
	void withoutAnEntityTheShapeIsLeftAlone() {
		assertSame(Shapes.block(), HostShapes.carve(null, new BlockPos(1, 2, 3), Shapes.block(), 5.0));
		assertSame(Shapes.block(), HostShapes.clipShape(null, new BlockPos(1, 2, 3), Shapes.block(), 5.0));
	}
}
