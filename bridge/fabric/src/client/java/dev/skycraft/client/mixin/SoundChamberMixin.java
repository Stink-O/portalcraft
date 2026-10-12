package dev.skycraft.client.mixin;

import dev.skycraft.link.Proto;
import dev.skycraft.link.SkyLink;
import net.minecraft.client.resources.sounds.SoundInstance;
import net.minecraft.client.sounds.SoundEngine;
import net.minecraft.sounds.SoundSource;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Unique;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * Portalcraft (bridge #23): in a Portal 2 chamber (HostAudio kAudioChamber) Minecraft plays only its local sounds:
 * blocks, players, hostile and neutral mobs, voice. Weather, ambient (wind, caves), music and records are muted, since
 * the chamber band sits in Minecraft's open sky. With kAudioLogSounds (tests) every sound started is logged with its
 * category and the host's level (test runs keep the client itself muted).
 */
@Mixin(SoundEngine.class)
public abstract class SoundChamberMixin {
	@Unique private static int logged;

	@Unique
	private static boolean portalcraft$mutedInChamber(SoundSource source) {
		return source == SoundSource.WEATHER || source == SoundSource.AMBIENT || source == SoundSource.MUSIC || source == SoundSource.RECORDS;
	}

	@Inject(method = "calculateVolume(FLnet/minecraft/sounds/SoundSource;)F", at = @At("RETURN"), cancellable = true)
	private void portalcraft$chamberCategories(float volume, SoundSource source, CallbackInfoReturnable<Float> cir) {
		if ((SkyLink.hostAudioFlags() & Proto.AUDIO_CHAMBER) != 0 && portalcraft$mutedInChamber(source)) {
			cir.setReturnValue(0.0F);
		}
	}

	@Inject(method = "play", at = @At("RETURN"))
	private void portalcraft$logSound(SoundInstance instance, CallbackInfoReturnable<?> cir) {
		int flags = SkyLink.hostAudioFlags();
		if ((flags & Proto.AUDIO_LOG_SOUNDS) == 0 || logged >= 400) {
			return;
		}
		logged++;
		boolean chamber = (flags & Proto.AUDIO_CHAMBER) != 0;
		SoundSource source = instance.getSource();
		float level = SkyLink.hostAudioLevel();
		float iv;
		try {
			iv = instance.getSound() == null ? 0.0F : instance.getVolume();  // (resolved by play; null for an unknown event)
		} catch (RuntimeException e) {
			iv = 0.0F;
		}
		float volume = chamber && portalcraft$mutedInChamber(source) ? 0.0F : Math.max(0.0F, Math.min(1.0F, iv)) * level;
		dev.skycraft.SkyCraft.LOG.info("Portalcraft: sound {} ({}) at {} {} {}: {}, host level {}, volume {} ({})", instance.getIdentifier(), source.getName(),
			String.format("%.1f", instance.getX()), String.format("%.1f", instance.getY()), String.format("%.1f", instance.getZ()), chamber ? "chamber" : "region",
			String.format("%.2f", level), String.format("%.2f", volume), cir.getReturnValue());
	}
}
