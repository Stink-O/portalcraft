package dev.skycraft;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertSame;

import net.minecraft.resources.Identifier;
import org.junit.jupiter.api.Test;

class LegacyIdsTest {
	@Test
	void oldNamespaceMapsToNew() {
		assertEquals(Identifier.fromNamespaceAndPath("portalcraft", "dug"), LegacyIds.remap(Identifier.fromNamespaceAndPath("skycraft", "dug")));
	}

	@Test
	void otherIdsAreUntouched() {
		Identifier a = Identifier.fromNamespaceAndPath("portalcraft", "dug");
		Identifier b = Identifier.fromNamespaceAndPath("minecraft", "stone");
		assertSame(a, LegacyIds.remap(a));
		assertSame(b, LegacyIds.remap(b));
	}
}
