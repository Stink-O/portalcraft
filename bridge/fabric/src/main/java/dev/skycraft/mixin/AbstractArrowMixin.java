package dev.skycraft.mixin;

import com.llamalad7.mixinextras.injector.wrapoperation.Operation;
import com.llamalad7.mixinextras.injector.wrapoperation.WrapOperation;
import dev.skycraft.SkyCraft;
import dev.skycraft.combat.SkyrimActorEntity;
import dev.skycraft.link.Proto;
import dev.skycraft.link.SkyLink;
import dev.skycraft.world.SkyClip;
import dev.skycraft.world.SkyCollision;
import dev.skycraft.world.SkyMovers;
import net.minecraft.util.Mth;
import net.minecraft.world.entity.LivingEntity;
import net.minecraft.world.entity.projectile.arrow.AbstractArrow;
import net.minecraft.world.entity.projectile.arrow.Arrow;
import net.minecraft.world.entity.projectile.arrow.SpectralArrow;
import net.minecraft.world.phys.EntityHitResult;
import net.minecraft.world.phys.Vec3;
import net.minecraft.world.level.block.state.BlockState;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.Unique;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;
import net.minecraft.world.level.ClipContext;
import net.minecraft.world.level.Level;
import net.minecraft.world.phys.BlockHitResult;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;

/**
 * Arrows and tridents hit Skyrim's exact surfaces. They then stick where they hit: the block state
 * there is air, the same as what they recorded on impact, so vanilla never makes them fall out.
 * <p>
 * Portalcraft (bridge #48): the same for Portal 2's chambers. The voxel walls and floors are in vanilla's own clip (ClipContextMixin), the
 * movers (doors, panels, lifts) are added here (SkyClip.refineProjectile), and an arrow stuck in a mover is carried with it. An arrow
 * that is stuck in host geometry falls again when that geometry goes (a dug cell), as one stuck in a broken block does.
 */
@Mixin(AbstractArrow.class)
public abstract class AbstractArrowMixin {
	@WrapOperation(
		method = "tick",
		at = @At(value = "INVOKE", target = "Lnet/minecraft/world/level/Level;clipIncludingBorder(Lnet/minecraft/world/level/ClipContext;)Lnet/minecraft/world/phys/BlockHitResult;")
	)
	private BlockHitResult skycraft$hitSkyrim(Level level, ClipContext context, Operation<BlockHitResult> original) {
		return SkyClip.refineProjectile(context.getFrom(), context.getTo(), original.call(level, context));
	}

	@Shadow
	private BlockState lastState;

	@Shadow
	protected abstract boolean isInGround();

	@Shadow
	protected abstract boolean shouldFall();

	@Shadow
	protected abstract void startFalling();

	@Shadow
	protected int inGroundTime;

	@Unique
	private static int portalcraft$logs;

	/** Where the arrow is stuck on its mover, if it is stuck in one. */
	@Unique
	private SkyMovers.Mount portalcraft$mount;

	@Inject(method = "onHitBlock", at = @At("TAIL"))
	private void portalcraft$stickInMover(BlockHitResult hit, CallbackInfo ci) {
		this.portalcraft$mount = null;
		AbstractArrow logged = (AbstractArrow) (Object) this;
		if (!logged.level().isClientSide() && portalcraft$logs < 120) {
			// (the testbed reads these lines from mc.log: what an arrow stuck in, and that it was still there later)
			portalcraft$logs++;
			SkyCraft.LOG.info("Portalcraft: arrow #{} hit {} at {} cell {} face {}", logged.getId(),
				hit instanceof SkyClip.MoverHitResult mh ? "mover " + mh.mover.moverId() : hit instanceof SkyClip.SkyrimHitResult ? "skyrim"
					: logged.level().getBlockState(hit.getBlockPos()).isAir() ? "host geometry" : "block " + logged.level().getBlockState(hit.getBlockPos()).getBlock(),
				hit.getLocation(), hit.getBlockPos().toShortString(), hit.getDirection());
		}
		if (hit instanceof SkyClip.MoverHitResult mover) {
			AbstractArrow self = (AbstractArrow) (Object) this;
			double yaw = Math.toRadians(self.getYRot()), pitch = Math.toRadians(self.getXRot());
			Vec3 dir = new Vec3(Math.sin(yaw) * Math.cos(pitch), Math.sin(pitch), Math.cos(yaw) * Math.cos(pitch));
			this.portalcraft$mount = SkyMovers.mount(mover.mover.moverId(), self.position(), dir);
		}
	}

	@Inject(method = "tick", at = @At("HEAD"))
	private void portalcraft$stuckInHost(CallbackInfo ci) {
		AbstractArrow self = (AbstractArrow) (Object) this;
		if (!this.isInGround()) {
			this.portalcraft$mount = null;
			return;
		}
		boolean server = !self.level().isClientSide();
		if (server && this.inGroundTime == 100 && portalcraft$logs < 120) {
			portalcraft$logs++;
			SkyCraft.LOG.info("Portalcraft: arrow #{} still stuck after 100 ticks at {}", self.getId(), self.position());
		}
		if (this.portalcraft$mount != null) {
			SkyMovers.Placed at = SkyMovers.place(this.portalcraft$mount);
			if (at == null) {
				this.portalcraft$mount = null; // its mover is gone
				if (server) {
					this.startFalling();
				}
				return;
			}
			if (at.pos().distanceToSqr(self.position()) > 1e-10) {
				self.setPos(at.pos().x, at.pos().y, at.pos().z);
				self.needsSync = true;
			}
			Vec3 d = at.dir();
			float yaw = (float) Math.toDegrees(Math.atan2(d.x, d.z)), pitch = (float) Math.toDegrees(Math.atan2(d.y, Math.sqrt(d.x * d.x + d.z * d.z)));
			if (Math.abs(Mth.wrapDegrees(yaw - self.getYRot())) > 0.01F || Math.abs(pitch - self.getXRot()) > 0.01F) {
				self.setYRot(yaw);
				self.setXRot(pitch);
				self.yRotO = yaw;
				self.xRotO = pitch;
				self.needsSync = true;
			}
		} else if (server && this.lastState != null && this.lastState.isAir() && SkyCollision.active() && SkyCollision.triangleCount() == 0 && (self.tickCount & 3) == 0 && this.shouldFall()) {
			// vanilla looks again only when the block state in the arrow's cell changes; host geometry has no state: look at the shape (a dug wall)
			SkyCraft.LOG.info("Portalcraft: arrow #{} at {} has nothing to hold it any more: it falls", self.getId(), self.position());
			this.startFalling();
		}
	}

	@Unique
	private Vec3 skycraft$hitAt;

	@Inject(method = "onHitEntity", at = @At("HEAD"))
	private void skycraft$rememberHit(EntityHitResult hitResult, CallbackInfo ci) {
		this.skycraft$hitAt = hitResult.getLocation();
	}

	/**
	 * Where Minecraft counts an arrow as stuck in a creature (it hurt it and didn't pierce): if that
	 * creature is a Skyrim NPC's stand-in, Skyrim pins the arrow to the NPC's skeleton.
	 */
	@WrapOperation(method = "onHitEntity", at = @At(value = "INVOKE", target = "Lnet/minecraft/world/entity/LivingEntity;setArrowCount(I)V"))
	private void skycraft$stickInSkyrimActor(LivingEntity mob, int count, Operation<Void> original) {
		original.call(mob, count);
		if (!(mob instanceof SkyrimActorEntity actor) || this.skycraft$hitAt == null || !SkyLink.active()) {
			return;
		}
		AbstractArrow self = (AbstractArrow) (Object) this;
		Vec3 v = self.getDeltaMovement();
		float yaw = (float) (Mth.atan2(v.x, v.z) * Mth.RAD_TO_DEG);
		float pitch = (float) (Mth.atan2(v.y, v.horizontalDistance()) * Mth.RAD_TO_DEG);
		int texture = self instanceof SpectralArrow ? 2 : self instanceof Arrow tippable && tippable.getColor() > 0 ? 1 : 0;
		Vec3 at = this.skycraft$hitAt;
		SkyLink.pushEvent(Proto.EV_ARROW_STUCK, actor.formId(), (float) at.x, (float) at.y, (float) at.z, yaw, Float.floatToRawIntBits(pitch), texture);
	}
}
