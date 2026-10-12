package dev.skycraft.world;

import static org.junit.jupiter.api.Assertions.*;

import dev.skycraft.link.Proto;
import net.minecraft.world.phys.AABB;
import net.minecraft.world.phys.Vec3;
import org.junit.jupiter.api.AfterEach;
import org.junit.jupiter.api.Test;

/** Bridge #43: the crosshair's ray against a mover's boxes at its pose, and the dig lattice cell it reports. */
class SkyMoversPickTest {
	@AfterEach
	void clean() {
		SkyMovers.clear();
	}

	private static final float[] LATTICE = {0, 0, 0, 0};

	@Test
	void aPhysicsPropBlocksTheRayButIsNotMinable() {
		// (bridge #50) lattice flags bit 1: solid and pickable (it blocks the ray, a block can be placed against it) but not minable
		SkyMovers.setForTest(6, 9, new AABB[] {new AABB(0, 0, 0, 1, 1, 1)}, 0, 0, 0, 0, 0, 0, 1, new float[] {Proto.LATTICE_NO_MINE, 0, 0, 0});
		SkyMovers.Hit h = SkyMovers.pick(new Vec3(0.5, 0.5, 5), new Vec3(0.5, 0.5, -5));
		assertNotNull(h);
		assertFalse(h.minable());
	}

	@Test
	void hitsThinPanelAndNamesTheCell() {
		SkyMovers.setForTest(5, 7, new AABB[] {new AABB(0, 0, 0, 1, 2, 0.1)}, 10, 0, 0, 0, 0, 0, 1, LATTICE);
		SkyMovers.Hit h = SkyMovers.pick(new Vec3(10.5, 1.5, 5), new Vec3(10.5, 1.5, -5));
		assertNotNull(h);
		assertEquals(5, h.moverId());
		assertEquals(0.1, h.z(), 1e-9);
		assertEquals(1.0, h.nz(), 1e-9);
		// local (0.5, 1.5, 0.1): the cell is (0, 1, 0): the thin panel lies in the first layer of the lattice
		assertEquals(0, h.cx());
		assertEquals(1, h.cy());
		assertEquals(0, h.cz());
		assertTrue(h.minable());
	}

	@Test
	void followsTheMoverPose() {
		double s = Math.sqrt(0.5);
		// turned 90 degrees about up: local (x, y, z) -> world (z, y, -x): the panel stands at world x 0..0.1, z -1..0
		SkyMovers.setForTest(5, 7, new AABB[] {new AABB(0, 0, 0, 1, 2, 0.1)}, 0, 0, 0, 0, s, 0, s, LATTICE);
		SkyMovers.Hit h = SkyMovers.pick(new Vec3(5, 1.5, -0.5), new Vec3(-5, 1.5, -0.5));
		assertNotNull(h);
		assertEquals(0.1, h.x(), 1e-6);
		assertEquals(1.0, h.nx(), 1e-6);
		assertEquals(0, h.cx());
		assertEquals(1, h.cy());
		assertEquals(0, h.cz());
	}

	@Test
	void looksThroughAHole() {
		// the panel with its cell (0, 0, 0) mined out: the shape is the two slabs left of it (x 1..2 and the upper layer)
		SkyMovers.setForTest(5, 8, new AABB[] {new AABB(1, 0, 0, 2, 2, 0.1), new AABB(0, 1, 0, 1, 2, 0.1)}, 0, 0, 0, 0, 0, 0, 1, LATTICE);
		assertNull(SkyMovers.pick(new Vec3(0.5, 0.5, 5), new Vec3(0.5, 0.5, -5)));
		SkyMovers.Hit h = SkyMovers.pick(new Vec3(1.5, 0.5, 5), new Vec3(1.5, 0.5, -5));
		assertNotNull(h);
		assertEquals(1, h.cx());
		// from inside the hole, sideways, the new face of the slab (a lattice plane) belongs to the cell the box is in
		h = SkyMovers.pick(new Vec3(0.5, 0.5, 0.05), new Vec3(2.5, 0.5, 0.05));
		assertNotNull(h);
		assertEquals(1.0, h.x(), 1e-9);
		assertEquals(-1.0, h.nx(), 1e-9);
		assertEquals(1, h.cx());
	}

	@Test
	void invisibleShapesAreNotHit() {
		SkyMovers.setForTest(5, 9, new AABB[] {new AABB(0, 0, 0, 1, 1, 1)}, 0, 0, 0, 0, 0, 0, 1, new float[] {1, 0, 0, 0});
		assertNull(SkyMovers.pick(new Vec3(0.5, 0.5, 5), new Vec3(0.5, 0.5, -5)));
	}

	@Test
	void aRayStartingInsideLooksPast() {
		SkyMovers.setForTest(5, 7, new AABB[] {new AABB(0, 0, 0, 2, 2, 2)}, 0, 0, 0, 0, 0, 0, 1, LATTICE);
		assertNull(SkyMovers.pick(new Vec3(1, 1, 1), new Vec3(1, 1, -5)));
	}
}
