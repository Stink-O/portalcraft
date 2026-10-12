package dev.skycraft.client.mixin;

import com.mojang.blaze3d.audio.Listener;
import com.mojang.blaze3d.audio.ListenerTransform;
import dev.skycraft.link.SkyLink;
import net.minecraft.client.Camera;
import net.minecraft.client.sounds.SoundEngine;
import net.minecraft.client.sounds.SoundEngineExecutor;
import net.minecraft.world.phys.Vec3;
import org.lwjgl.openal.AL10;
import org.spongepowered.asm.mixin.Final;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.Unique;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/**
 * Portalcraft: Minecraft hears from where the host's player hears it (HostAudio, protocol v14): the
 * listener is the host's pose, never the hidden player's camera, and the host sets Minecraft's overall
 * level (OpenAL listener gain; 0 outside the region and away from its portals). Only with
 * -Dskycraft.audio=true (user sessions); test runs keep the hidden client muted.
 */
@Mixin(SoundEngine.class)
public abstract class SoundEngineListenerMixin {
	@Shadow private boolean loaded;
	@Shadow @Final private Listener listener;
	@Shadow @Final private SoundEngineExecutor executor;

	@Unique private static final boolean AUDIO = Boolean.getBoolean("skycraft.audio");
	@Unique private static final double[] POS = new double[3];
	@Unique private static final float[] YPG = new float[3];
	@Unique private static long lastLog;

	@Inject(method = "updateSource", at = @At("HEAD"), cancellable = true)
	private void portalcraft$hostListener(Camera camera, CallbackInfo ci) {
		if (!AUDIO || !this.loaded) {
			return;
		}
		ci.cancel();
		float gain = 0.0F;
		ListenerTransform transform = null;
		if (SkyLink.readHostAudio(POS, YPG)) {
			gain = Math.max(0.0F, Math.min(1.0F, YPG[2]));
			double yaw = Math.toRadians(YPG[0]), pitch = Math.toRadians(YPG[1]);
			Vec3 forward = new Vec3(-Math.sin(yaw) * Math.cos(pitch), -Math.sin(pitch), Math.cos(yaw) * Math.cos(pitch));
			Vec3 right = forward.cross(new Vec3(0, 1, 0));
			if (right.lengthSqr() < 1e-6) {
				right = new Vec3(-Math.cos(yaw), 0, -Math.sin(yaw));
			}
			Vec3 up = right.normalize().cross(forward).normalize();
			transform = new ListenerTransform(new Vec3(POS[0], POS[1], POS[2]), forward, up);
		}
		final ListenerTransform t = transform;
		final float g = gain;
		this.executor.execute(() -> {
			if (t != null) {
				this.listener.setTransform(t);
			}
			AL10.alListenerf(AL10.AL_GAIN, g);
		});
		long now = System.currentTimeMillis();
		if (t != null && now - lastLog > 5000) {
			lastLog = now;
			dev.skycraft.SkyCraft.LOG.info("Portalcraft: audio listener at {} {} {}, gain {}", String.format("%.1f", POS[0]), String.format("%.1f", POS[1]), String.format("%.1f", POS[2]), g);
		}
	}
}
