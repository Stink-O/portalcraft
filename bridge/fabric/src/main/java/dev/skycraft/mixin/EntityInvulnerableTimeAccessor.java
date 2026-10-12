package dev.skycraft.mixin;

import net.minecraft.world.entity.Entity;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.gen.Accessor;

// Portalcraft (bridge #55): Entity.invulnerableTime is private in 26.3; SkyCombat clears it so every Portal 2 hit lands.
@Mixin(Entity.class)
public interface EntityInvulnerableTimeAccessor {
	@Accessor("invulnerableTime")
	void skycraft$setInvulnerableTime(int ticks);
}
