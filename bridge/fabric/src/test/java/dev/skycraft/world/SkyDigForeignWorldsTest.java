package dev.skycraft.world;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.util.List;
import org.junit.jupiter.api.Test;

/** (bridge #46) A chunk keeps one map's dug cells apart from the others': the other maps' sections can be dropped in one go. */
class SkyDigForeignWorldsTest {
	private static SkyDig.DugSection section(int world, int y) {
		long[] bits = new long[64];
		DigMath.setBit(bits, 5);
		return new SkyDig.DugSection(world, y, bits);
	}

	@Test
	void withoutWorldsKeepsOnlyTheCurrentMap() {
		SkyDig.DugColumn column = new SkyDig.DugColumn(List.of(section(1, 0), section(2, 0), section(2, -1), section(3, 4)));
		SkyDig.DugColumn mine = column.withoutWorlds(w -> w != 2);
		assertEquals(2, mine.sections().size());
		assertTrue(mine.hasWorld(2));
		assertFalse(mine.hasWorld(1));
		assertFalse(mine.hasWorld(3));
		assertTrue(column.hasWorldWhere(w -> w != 2));
		assertFalse(mine.hasWorldWhere(w -> w != 2));
	}

	@Test
	void aColumnOfOneMapIsLeftAlone() {
		SkyDig.DugColumn column = new SkyDig.DugColumn(List.of(section(7, 0), section(7, 1)));
		assertFalse(column.hasWorldWhere(w -> w != 7));
		assertEquals(column.sections(), column.withoutWorlds(w -> w != 7).sections());
	}

	@Test
	void everyWorldGoneIsTheEmptyColumn() {
		SkyDig.DugColumn column = new SkyDig.DugColumn(List.of(section(1, 0)));
		assertSame(SkyDig.DugColumn.EMPTY, column.withoutWorlds(w -> true));
	}

	@Test
	void visibilityFieldsOfTheHostsPad() {
		int bits = SkyCollision.visFill(0); // an older host: nothing known, the whole cell counts
		assertEquals(0, bits);
		int pad = dev.skycraft.link.Proto.COL_BLOCK_VIS_KNOWN | (27 << dev.skycraft.link.Proto.COL_BLOCK_VIS_SAMPLES_SHIFT);
		assertTrue(SkyCollision.visFill(pad) != 0);
		int ghost = dev.skycraft.link.Proto.COL_BLOCK_VIS_KNOWN; // known, nothing visible
		assertTrue((SkyCollision.visFill(ghost) & (1 << 18)) == 0);
		assertTrue((SkyCollision.visFill(pad | dev.skycraft.link.Proto.COL_BLOCK_VIS_ANY) & (1 << 18)) != 0);
		assertEquals(27, (SkyCollision.visFill(pad) >>> 19) & 31);
	}
}
