package dev.skycraft.mixin;

import dev.skycraft.LegacyIds;
import net.fabricmc.fabric.impl.attachment.AttachmentRegistryImpl;
import net.minecraft.resources.Identifier;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.ModifyVariable;

/** Chunks saved before the skycraft -> portalcraft rename hold the dug blocks as skycraft:dug; read them as portalcraft:dug. */
@Mixin(value = AttachmentRegistryImpl.class, remap = false)
public class LegacyAttachmentIdMixin {
	@ModifyVariable(method = "get", at = @At("HEAD"), argsOnly = true, remap = false)
	private static Identifier portalcraft$legacyId(Identifier id) {
		return LegacyIds.remap(id);
	}
}
