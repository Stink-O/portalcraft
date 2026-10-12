package dev.skycraft.client;

import dev.skycraft.SkyCraft;
import dev.skycraft.client.render.WorldExporter;
import dev.skycraft.link.Proto;
import dev.skycraft.link.SkyLink;
import dev.skycraft.world.SkyCollision;
import net.minecraft.client.Camera;
import net.minecraft.client.Minecraft;
import net.minecraft.client.player.LocalPlayer;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.phys.Vec3;
import org.lwjgl.sdl.SDLVideo;

/**
 * Per-frame glue between the Minecraft client and Skyrim. Everything here runs on the render
 * thread, called from MinecraftMixin.
 */
public final class SkyClient {
	private static final boolean SHOW_WINDOW = Boolean.getBoolean("skycraft.showWindow");
	// Started by Skyrim (SkyCraft's bundled instance passes -Dskycraft.startHidden=true): no window and
	// no title-screen music from the first frame, even while Skyrim is paused (Alt-Tabbed) and the
	// two haven't linked up yet. Otherwise the window only goes once Skyrim is there.
	private static final boolean START_HIDDEN = Boolean.getBoolean("skycraft.startHidden");
	private static boolean startedHidden;

	private static final SkyLink.SkyState sky = new SkyLink.SkyState();
	private static final SkyLink.McState mc = new SkyLink.McState();
	private static volatile boolean linked;
	private static boolean tookOver;
	private static boolean windowHidden;
	private static int appliedViewportW, appliedViewportH;

	// Teleport / hold state: Skyrim decides where the player is after loads, doors and respawns.
	private static int lastTeleportSeq = -1;
	private static int teleportAck;
	private static boolean teleportPending;
	private static LocalPlayer lastPlayer;
	private static Vec3 holdPos;
	private static Vec3 unlinkedHold;
	private static long holdSince;
	// Portalcraft (bridge #13): the host's velocity for the player at the pending teleport (a switch to the Minecraft
	// player mid-fall, a portal transit); applied when the hold releases
	private static volatile Vec3 pendingVelocity;

	public static void setPendingVelocity(double x, double y, double z) {
		pendingVelocity = new Vec3(x, y, z);
	}
	private static long qpcFreq;
	private static LocalPlayer eyePlayer;
	private static float eyeSmoothed;
	private static long frameCounter;
	private static int lastPacedSeq;
	private static boolean skyrimStalled;
	private static int exporterErrors;

	private SkyClient() {
	}

	public static boolean linked() {
		return linked;
	}

	/**
	 * True once Skyrim has connected in this session. From then on Minecraft never touches the
	 * real mouse or keyboard again (even if Skyrim closes), since its window is hidden.
	 */
	public static boolean tookOver() {
		return tookOver;
	}

	public static SkyLink.SkyState sky() {
		return sky;
	}

	/** Start of Minecraft.runTick: pull state and input from Skyrim before anything else runs. */
	private static long frameStartNs, statStartNs, statFrames, statWorkNs;

	public static void beginFrame() {
		frameStartNs = System.nanoTime();
		SkyLink.poll();
		dev.skycraft.client.portalcraft.HudExporter.update();
		quitWithSkyrim(Minecraft.getInstance());
		if (START_HIDDEN && !startedHidden) {
			startedHidden = true;
			Minecraft minecraft = Minecraft.getInstance();
			hideWindowOnce(minecraft);
			minecraft.options.getSoundSourceOptionInstance(net.minecraft.sounds.SoundSource.MUSIC).set(0.0);
			// Portalcraft: a hidden Minecraft is silent in test runs. In a user session
			// (-Dskycraft.audio=true) it plays at full master volume and the host drives its listener
			// and overall level (SoundEngineListenerMixin).
			minecraft.options.getSoundSourceOptionInstance(net.minecraft.sounds.SoundSource.MASTER).set(Boolean.getBoolean("skycraft.audio") ? 1.0 : 0.0);
			// Portalcraft: the render distance comes from -Dskycraft.renderDistance (run_mc.sh), not
			// options.txt, from the first frame, so chunks load at that radius before the host links.
			int rd = Integer.getInteger("skycraft.renderDistance", 8);
			minecraft.options.renderDistance().set(rd);
			minecraft.options.simulationDistance().set(Math.min(rd, 10));
			minecraft.options.save();
			minecraft.getMusicManager().stopPlaying();
		}
		boolean nowLinked = SkyLink.active();
		if (nowLinked) {
			SkyLink.readSkyState(sky); // on a torn read we simply keep last frame's state
			dev.skycraft.world.SkyWater.refresh();
		} else {
			dev.skycraft.world.SkyWater.clear();
		}
		if (nowLinked != linked) {
			linked = nowLinked;
			SkyCraft.LOG.info("SkyCraft: Skyrim link {}", linked ? "up" : "down");
			if (linked) {
				tookOver = true;
				unlinkedHold = null;
				SkyCollision.startConsumer();
				applyLinkedOptions();
			} else {
				InputBridge.releaseAll();
				LocalPlayer player = Minecraft.getInstance().player;
				unlinkedHold = player != null ? player.position() : null;
			}
		}
		if (!linked) {
			return;
		}

		Minecraft minecraft = Minecraft.getInstance();
		hideWindowOnce(minecraft);
		applyViewportSize(minecraft);
		dev.skycraft.client.portalcraft.HostViewClient.poll(minecraft);
		dev.skycraft.client.portalcraft.LightProbes.frame(minecraft);
		dev.skycraft.client.portalcraft.McProfiler.frame(minecraft);
		dev.skycraft.client.portalcraft.TestReset.frame(minecraft);  // bridge #19: tests start (and end) alive
		dev.skycraft.client.portalcraft.WorldHash.frame(minecraft);
		MirrorWorld.openWhenReady(minecraft);

		if (sky.menuOpen() || sky.loading()) {
			InputBridge.releaseAll();
		}
		InputBridge.drain(minecraft);
		ProxySync.frame(minecraft);

		LocalPlayer player = minecraft.player;
		if (player == null) {
			lastPlayer = null;
			return;
		}

		// A new player object means we just joined or respawned: put it where Skyrim's player is.
		if (player != lastPlayer) {
			lastPlayer = player;
			teleportPending = true;
		}
		if (sky.teleportSeq != lastTeleportSeq) {
			lastTeleportSeq = sky.teleportSeq;
			teleportPending = true;
			dev.skycraft.client.portalcraft.AvatarHost.refreshFlags();  // (bridge #34) the hand-over flag is read fresh for this teleport
		}
		if (teleportPending && sky.inGame() && !sky.loading()) {
			// (bridge #28: with a host that sets HP_MC_TRANSIT a portal transit is Minecraft's own (PlayerTransit) and every host
			// teleport holds until the collision is in. LEGACY, for hosts before #28 only (they transit the body in Portal 2 and send a
			// moving teleport): bridge #20's soft teleport. Delete with softTeleport once no pre-#28 build is played.)
			Vec3 pv = pendingVelocity;
			int bx = (int) Math.floor(sky.x), by = (int) Math.floor(sky.y), bz = (int) Math.floor(sky.z);
			boolean moving = pv != null && pv.lengthSqr() > 1e-4;
			boolean known = SkyCollision.isKnown(bx, by - 1, bz) && SkyCollision.isKnown(bx, by, bz);
			if (!dev.skycraft.client.portalcraft.AvatarHost.hostWantsTransit() && moving && known && lastPlayer == player && holdPos == null) {
				softTeleport(minecraft, sky.x, sky.y, sky.z, pv);
				pendingVelocity = null;
				teleportAck = sky.teleportSeq;
				teleportPending = false;
			} else {
				requestTeleport(minecraft, sky.x, sky.y, sky.z, sky.yaw, sky.pitch);
				teleportAck = sky.teleportSeq;
				teleportPending = false;
				holdPos = new Vec3(sky.x, sky.y, sky.z);
				transitHold = false;
			}
		}

		// Look direction is driven by Skyrim (zero-latency camera); MC uses it for everything else.
		if (minecraft.gui.screen() == null) {
			// (bridge #28) turned by a portal transit the host has not turned its own look for yet (SkyState transitAck)
			float[] look = dev.skycraft.client.portalcraft.PlayerTransit.look(sky.transitAck, sky.yaw, sky.pitch);
			// (bridge #30) the yaw nearest the player's own: a host yaw that wrapped (350 -> -10) or a transit's turn must not step the
			// player's yaw by 360, which the hand sway (bob chasing the yaw) would swing through
			look[0] = player.getYRot() + (float) Math.IEEEremainder(look[0] - player.getYRot(), 360.0);
			player.setYRot(look[0]);
			player.setXRot(look[1]);
			player.yRotO = look[0];
			player.xRotO = look[1];
		}
	}

	// Minecraft is started with Skyrim (the SKSE plugin launches it), so it goes when that Skyrim has
	// closed for good: saved and shut down the normal way. -Dskycraft.quitWithSkyrim=false keeps it
	// running instead (development: restarting Skyrim without restarting Minecraft).
	private static final boolean QUIT_WITH_SKYRIM = Boolean.parseBoolean(System.getProperty("skycraft.quitWithSkyrim", "true"));
	private static long skyrimGoneSince;
	private static long nextSkyrimCheck;

	private static void quitWithSkyrim(Minecraft minecraft) {
		int pid = SkyLink.skyrimPid();
		long now = System.currentTimeMillis();
		if (!QUIT_WITH_SKYRIM || pid == 0 || now < nextSkyrimCheck) {
			return;
		}
		nextSkyrimCheck = now + 1000;
		if (ProcessHandle.of(pid).map(ProcessHandle::isAlive).orElse(false)) {
			skyrimGoneSince = 0;
			return;
		}
		if (skyrimGoneSince == 0) {
			skyrimGoneSince = now;
		} else if (now - skyrimGoneSince > 5000) {
			SkyCraft.LOG.info("SkyCraft: Skyrim (pid {}) has closed; saving and quitting", pid);
			minecraft.stop();
		}
	}

	/** Called at the end of every client tick. */
	public static void clientTick(Minecraft minecraft) {
		MirrorWorld.tick(minecraft);
		DiscordPresence.tick(minecraft);
		freezeWhileUnlinked(minecraft);
		holdUntilReady(minecraft);
		dev.skycraft.client.portalcraft.PlayerTransit.tick(minecraft, mc, holdPos != null || !linked);  // bridge #28
		publishTick(minecraft);
	}

	/**
	 * Skyrim went quiet (a long loading screen, a stall, or it closed). Its collision around the
	 * player may be about to change (interior doors), so keep the player exactly where they were
	 * instead of letting them fall; Skyrim puts them where they belong when it's back.
	 */
	private static void freezeWhileUnlinked(Minecraft minecraft) {
		LocalPlayer player = minecraft.player;
		if (linked || !tookOver || player == null) {
			return;
		}
		if (unlinkedHold == null) {
			unlinkedHold = player.position();
		}
		player.setDeltaMovement(Vec3.ZERO);
		player.setPos(unlinkedHold.x, unlinkedHold.y, unlinkedHold.z);
		player.xo = unlinkedHold.x;
		player.yo = unlinkedHold.y;
		player.zo = unlinkedHold.z;
		player.resetFallDistance();
	}

	/**
	 * Hands Skyrim the raw physics tick (previous + latest feet, smoothed eye height, walk bob) with a
	 * QueryPerformanceCounter timestamp. Skyrim interpolates between them on its own frame clock,
	 * exactly like Minecraft's renderer does with partial ticks.
	 */
	private static void publishTick(Minecraft minecraft) {
		LocalPlayer player = minecraft.player;
		if (!linked || player == null) {
			return;
		}
		if (qpcFreq == 0) {
			qpcFreq = SkyLink.qpcFrequency();
		}
		float tickMs = minecraft.level != null ? minecraft.level.tickRateManager().millisecondsPerTick() : 50.0F;
		// The tick really "happened" partial ticks ago (DeltaTracker keeps the remainder).
		float remainder = minecraft.getDeltaTracker().getGameTimeDeltaPartialTick(false);
		mc.tickQpc = SkyLink.qpc() - (long) (remainder * tickMs * qpcFreq / 1000.0);
		mc.tickMs = tickMs;
		mc.prevX = player.xo;
		mc.prevY = player.yo;
		mc.prevZ = player.zo;
		mc.curX = player.getX();
		mc.curY = player.getY();
		mc.curZ = player.getZ();
		// Same smoothing as Camera.tick(): eye height eases halfway toward the target each tick.
		if (player != eyePlayer) {
			eyePlayer = player;
			eyeSmoothed = player.getEyeHeight();
		}
		mc.eyeHeightO = eyeSmoothed;
		eyeSmoothed += (player.getEyeHeight() - eyeSmoothed) * 0.5F;
		mc.eyeHeightT = eyeSmoothed;
		boolean bob = minecraft.options.bobView().get();
		var avatar = player.avatarState();
		mc.walkDistO = bob ? avatar.getInterpolatedWalkDistance(0.0F) : 0.0F;
		mc.walkDist = bob ? avatar.getInterpolatedWalkDistance(1.0F) : 0.0F;
		mc.bobO = bob ? avatar.getInterpolatedBob(0.0F) : 0.0F;
		mc.bob = bob ? avatar.getInterpolatedBob(1.0F) : 0.0F;
		// (bridge #26) the mover this tick rode (SkyMovers): the host keeps the feet on it as it moves now
		mc.rideMover = dev.skycraft.world.SkyMovers.rideMover;
		mc.rideFlags = dev.skycraft.world.SkyMovers.rideFlags;
		mc.ridePrevX = dev.skycraft.world.SkyMovers.ridePrevX;
		mc.ridePrevY = dev.skycraft.world.SkyMovers.ridePrevY;
		mc.ridePrevZ = dev.skycraft.world.SkyMovers.ridePrevZ;
		mc.rideCurX = dev.skycraft.world.SkyMovers.rideCurX;
		mc.rideCurY = dev.skycraft.world.SkyMovers.rideCurY;
		mc.rideCurZ = dev.skycraft.world.SkyMovers.rideCurZ;
		SkyLink.writeMcState(mc);
	}

	private static int holdLogTicks = 0;  // (bridge #40) the first 400 held ticks are logged (body vs held position vs the car's pose)
	private static int ackHoldTicks = 0;
	private static boolean handOverHold = false;

	/** Freeze the player until Skyrim's collision around them has arrived. */
	private static void holdUntilReady(Minecraft minecraft) {
		LocalPlayer player = minecraft.player;
		if (!linked || player == null) {
			return;
		}
		if (!sky.inGame() || sky.loading()) {
			// Skyrim is on its main menu or a loading screen: park the player where they are.
			if (holdPos == null) {
				holdPos = player.position();
			}
			teleportPending = true;
		}
		if (holdPos == null) {
			holdSince = 0;
			if (ackHoldTicks > 0) {
				ackHoldTicks--;
			}
			return;
		}
		if (holdSince == 0) {
			holdSince = System.currentTimeMillis();
			handOverHold = false;
		}
		// (bridge #31) the host is handing its body over and the movers around it (an elevator car it rides) are not in Minecraft yet:
		// stay on the body wherever it goes (the host's position is its feet now), and do not release on static ground far below
		// (a car rising out of a shaft: the player fell out of it to the shaft's floor)
		final boolean handOver = dev.skycraft.client.portalcraft.AvatarHost.hostHandOver();
		if (handOver) {
			handOverHold = true;
			Vec3 body = new Vec3(sky.x, sky.y, sky.z);
			if (body.distanceToSqr(holdPos) > 9.0) {
				requestTeleport(minecraft, sky.x, sky.y, sky.z, sky.yaw, sky.pitch);
			}
			holdPos = body;
		}
		// (bridge #26) held on a mover: the hold is in its frame (an elevator descends on while the teleport waits)
		if (!handOver) {
			holdPos = dev.skycraft.world.SkyMovers.carryHeld(player, holdPos);
		}
		int bx = (int) Math.floor(holdPos.x), by = (int) Math.floor(holdPos.y), bz = (int) Math.floor(holdPos.z);
		boolean known = SkyCollision.isKnown(bx, by - 1, bz) && SkyCollision.isKnown(bx, by, bz)
			&& SkyCollision.isKnown(bx, by - SkyCollision.REGION_SIZE, bz);
		// Release once there is actual ground below (or after a timeout, e.g. when mid-air on purpose).
		Vec3 pv = pendingVelocity;
		boolean moving = pv != null && pv.lengthSqr() > 1e-4;
		boolean ready = known && (SkyCollision.hasSolidBelow(bx, by, bz, 12) || moving || System.currentTimeMillis() - holdSince > 6000);
		// (bridge #26) over a host mover (an elevator car, a lift panel): its collision holds the player up
		if (!ready && known && !Double.isNaN(dev.skycraft.world.SkyMovers.topNear(holdPos.x, holdPos.y, holdPos.z, 12.0, 0.75))) {
			ready = true;
		}
		// Portalcraft (bridge #13): on Minecraft's own terrain (a loaded chunk inside the build range) the ground is
		// Minecraft's: release once a block is under the player, or at once when the host gave it a velocity (a switch
		// mid-fall, a portal transit). The host exports no collision there, so the SkyCollision test above never passes.
		var level = minecraft.level;
		if (!ready && level != null && by >= level.getMinY() && by < level.getMaxY() && level.hasChunk(bx >> 4, bz >> 4)) {
			boolean ground = false;
			for (int d = 0; d <= 12 && !ground; d++) {
				ground = !level.getBlockState(new net.minecraft.core.BlockPos(bx, by - d, bz)).getCollisionShape(level, new net.minecraft.core.BlockPos(bx, by - d, bz)).isEmpty();
			}
			ready = ground || moving || System.currentTimeMillis() - holdSince > 2000;
		}
		// (and while a mover the host poses has no shape here yet: the collision ring is read on its own thread; at most 4 s more)
		if (handOver || (dev.skycraft.world.SkyMovers.unresolved > 0 && System.currentTimeMillis() - holdSince < 10000)) {
			ready = false;
		}
		// (bridge #34) a host teleport is held for at least 100 ms (two client ticks): the host writes its hand-over flag before the teleport it belongs to,
		// and a fresh read of it is made for each new teleport, but the shapes of the movers around the body are posed in the same frame; the extra ticks
		// are the margin for the collision thread to have read them. (A portal transit's exit hold is not delayed: it carries velocity and must go.)
		if (!transitHold && System.currentTimeMillis() - holdSince < 100) {
			ready = false;
		}
		if (ready && sky.inGame() && !sky.loading()) {
			// Portalcraft (bridge #13): released while moving (a switch mid-fall, a transit): start from where the host's
			// body is now, not where it was when the teleport was asked for (the hold lasts a few frames, the body falls on)
			if (moving && sky.teleportSeq == lastTeleportSeq && !transitHold) {
				Vec3 now = new Vec3(sky.x, sky.y, sky.z);
				if (now.distanceToSqr(holdPos) > 1e-4) {
					requestTeleport(minecraft, sky.x, sky.y, sky.z, sky.yaw, sky.pitch);
					holdPos = now;
				}
			}
			// Skyrim's feet can sit a fraction of a voxel inside our ground layer. Minecraft's
			// collision never pushes you out of a shape, so you'd drop through: lift out first.
			Vec3 safe = liftOutOfGeometry(player, holdPos);
			if (handOverHold) {
				Vec3 on = dev.skycraft.world.SkyMovers.standOnRelease(player, safe);  // (bridge #40) exactly on the top of the mover the body stands on
				if (on.y != safe.y) {
					SkyCraft.LOG.info("SkyCraft: hand-over: put {} blocks {} onto the mover's top", String.format("%.3f", Math.abs(on.y - safe.y)), on.y > safe.y ? "up" : "down");
				}
				safe = on;
			}
			if (safe.y != holdPos.y) {
				player.setPos(safe.x, safe.y, safe.z);
				player.yo = safe.y;
				SkyCraft.LOG.info("SkyCraft: lifted player {} blocks out of the ground", String.format("%.3f", safe.y - holdPos.y));
			}
			holdPos = null;
			ackHoldTicks = handOverHold ? 2 : 0;
			transitHold = false;
			if (pv != null) {
				player.setDeltaMovement(pv);
				pendingVelocity = null;
				SkyCraft.LOG.info("SkyCraft: released with the host's velocity {} {} {} blocks/tick", String.format("%.3f", pv.x), String.format("%.3f", pv.y), String.format("%.3f", pv.z));
			}
			return;
		}
		if (handOver && holdLogTicks++ < 400) {
			SkyCraft.LOG.info("SkyCraft: hold {} body y {} hold y {} player y {} handOver {} known {} ready {} ride {} rideCur y {} unresolved {} topNear {}", holdLogTicks,
				String.format("%.3f", sky.y), String.format("%.3f", holdPos.y), String.format("%.3f", player.getY()), handOver, known, ready, dev.skycraft.world.SkyMovers.rideMover,
				String.format("%.3f", dev.skycraft.world.SkyMovers.rideCurY), dev.skycraft.world.SkyMovers.unresolved,
				String.format("%.3f", dev.skycraft.world.SkyMovers.topNear(holdPos.x, holdPos.y, holdPos.z, 12.0, 0.75)));
		}
		player.setDeltaMovement(Vec3.ZERO);
		player.setPos(holdPos.x, holdPos.y, holdPos.z);
		player.xo = holdPos.x;
		player.yo = holdPos.y;
		player.zo = holdPos.z;
		player.resetFallDistance();
	}

	private static Vec3 liftOutOfGeometry(LocalPlayer player, Vec3 pos) {
		// Stand on the exact Skyrim ground if it is slightly above the feet (up to 2.5 blocks).
		double ground = SkyCollider.groundAt(pos.x, pos.y, pos.z, 2.5);
		// (bridge #26) or a mover's top up to 0.75 block above the feet (the host's feet on its true floor, a mover's box a
		// fraction higher): overlaps are pushed out by SkyMovers too, but only up when the player stood on the ground
		double top = dev.skycraft.world.SkyMovers.topNear(pos.x, pos.y, pos.z, 0.0, 0.75);
		if (!Double.isNaN(top) && (Double.isNaN(ground) || top > ground)) {
			ground = top;
		}
		Vec3 out = !Double.isNaN(ground) && ground > pos.y ? new Vec3(pos.x, ground, pos.z) : pos;
		// (bridge #63) the host's collision is a voxel field, not triangles: the feet of a body standing on a floor whose true height is a little under
		// the voxels' top (the export rounds a 1-voxel slab up, sp_a1_intro1's wake-up room: feet 2 u inside it) are inside a shape. Minecraft ignores a
		// shape whose top is above the box's bottom, so the player fell through the slab. Lifted by the least amount that frees the box (at most 0.5 block).
		var level = player.level();
		if (level != null) {
			net.minecraft.world.phys.AABB box = player.getBoundingBox().move(out.x - player.getX(), out.y - player.getY(), out.z - player.getZ());
			if (!level.noCollision(player, box)) {
				for (int i = 1; i <= 32; i++) {
					double dy = i / 64.0;
					if (level.noCollision(player, box.move(0, dy, 0))) {
						SkyCraft.LOG.info("SkyCraft: feet {} blocks inside the host's collision: lifted onto it", String.format("%.3f", dy));
						return new Vec3(out.x, out.y + dy, out.z);
					}
				}
			}
		}
		return out;
	}

	/**
	 * Portalcraft (bridge #28): a portal transit whose exit has no collision in Minecraft yet (PlayerTransit): the player waits there,
	 * with the transit's velocity, until it has (holdUntilReady). The host prefetches the far side of a portal the player is near, so
	 * this is a bug when it happens (logged).
	 */
	/** LEGACY (pre-#28 hosts only, see beginFrame): Portalcraft (bridge #20): a moving teleport: no hold, no packet, the velocity kept. */
	private static void softTeleport(Minecraft minecraft, double x, double y, double z, Vec3 displacement) {
		LocalPlayer player = minecraft.player;
		// (bridge #22) feet a fraction inside a host floor voxel (the exit sits a few units into the export's 1-block
		// rounding) go on its top now: Minecraft lifted them a few ticks later, a 6 u pop in the view after a sprint
		var feet = net.minecraft.core.BlockPos.containing(x, y, z);
		var shape = SkyCollision.shapeAt(feet);
		if (shape != null && !shape.isEmpty()) {
			double top = feet.getY() + shape.max(net.minecraft.core.Direction.Axis.Y);
			if (top > y && top - y < 0.35) {
				y = top;
			}
		}
		player.setPos(x, y, z);
		player.setOldPosAndRot();
		player.resetFallDistance();
		// (bridge #22) the host sends the last tick's displacement (blocks/tick); Minecraft's deltaMovement is that tick's
		// displacement after friction (ground: block x 0.91; air: 0.91), vertical: gravity and drag
		boolean ground = !player.level().noCollision(player, player.getBoundingBox().move(0, -0.08, 0));
		double f = ground ? player.level().getBlockState(feet.below()).getBlock().getFriction() * 0.91 : 0.91;
		Vec3 velocity = new Vec3(displacement.x * f, (displacement.y - 0.08) * 0.98, displacement.z * f);
		player.setDeltaMovement(velocity);
		// (bridge #22) the state shipped this frame carries the ack: it must carry the new spot too
		mc.prevX = x;
		mc.prevY = y;
		mc.prevZ = z;
		mc.curX = x + displacement.x;
		mc.curY = y + displacement.y;
		mc.curZ = z + displacement.z;
		if (qpcFreq != 0) {
			mc.tickQpc = SkyLink.qpc();
		}
		final double fy = y;
		var server = minecraft.getSingleplayerServer();
		if (server != null) {
			var uuid = player.getUUID();
			server.execute(() -> {
				ServerPlayer sp = server.getPlayerList().getPlayer(uuid);
				if (sp != null) {
					sp.absSnapTo(x, fy, z, sp.getYRot(), sp.getXRot());
					sp.setDeltaMovement(velocity);
					sp.resetFallDistance();
					sp.connection.resetPosition();
				}
			});
		}
		SkyCraft.LOG.info("SkyCraft: moved to {} {} {} with velocity {} {} {} (no hold, {}, displacement {} {} {})", String.format("%.2f", x), String.format("%.2f", y), String.format("%.2f", z),
			String.format("%.3f", velocity.x), String.format("%.3f", velocity.y), String.format("%.3f", velocity.z), ground ? "ground" : "air",
			String.format("%.3f", displacement.x), String.format("%.3f", displacement.y), String.format("%.3f", displacement.z));
	}

	private static boolean transitHold;

	public static void holdAfterTransit(Vec3 pos, Vec3 velocity) {
		transitHold = true;
		holdPos = pos;
		pendingVelocity = velocity;
	}

	private static void requestTeleport(Minecraft minecraft, double x, double y, double z, float yaw, float pitch) {
		LocalPlayer player = minecraft.player;
		dev.skycraft.portalcraft.PlayerFling.end();  // (bridge #28) a host teleport ends a fling
		if (player.position().distanceToSqr(x, y, z) > 4.0) {
			dev.skycraft.world.SkyMovers.forget(player);  // (bridge #36) a teleport away from the mover it rode: its motion does not carry the player at the new spot
		}
		player.setPos(x, y, z);
		player.setOldPosAndRot();  // (Portalcraft bridge #20: no interpolation across a teleport)
		player.setDeltaMovement(Vec3.ZERO);
		player.resetFallDistance();
		var server = minecraft.getSingleplayerServer();
		if (server != null) {
			var uuid = player.getUUID();
			server.execute(() -> {
				ServerPlayer sp = server.getPlayerList().getPlayer(uuid);
				if (sp != null) {
					sp.teleportTo(x, y, z);
					sp.setYRot(yaw);
					sp.setXRot(pitch);
					sp.resetFallDistance();
				}
			});
		}
		SkyCraft.LOG.info("SkyCraft: teleported to {} {} {}", x, y, z);
	}

	private static String expectedChunkList(Minecraft mc) {
		var it = mc.levelRenderer.sectionOcclusionGraph().expectedChunks().iterator();
		StringBuilder b = new StringBuilder("[");
		for (int i = 0; i < 3 && it.hasNext(); i++) {
			long k = it.nextLong();
			b.append(net.minecraft.world.level.ChunkPos.getX(k)).append(',').append(net.minecraft.world.level.ChunkPos.getZ(k)).append(' ');
		}
		return b.append(']').toString();
	}

	// (2026-10-12) where a frame's time goes besides tick + render: the export (afterRender's body), Minecraft's own swap and housekeeping
	// (afterRender's end to paceFrame), the wait for the host's next frame (paceFrame), and how often the "host stalled" wait ran. On Windows
	// the hidden Minecraft sat at 20-26 fps with 7 ms of work a frame, a pace the user sees as reprojection artifacts.
	private static long statExportNs, statSwapNs, statPaceNs, afterRenderEndNs;
	private static int statStalls;

	/** After GameRenderer.render(): report the player to Skyrim and ship the overlay frame. */
	public static void afterRender() {
		long t0 = System.nanoTime();
		try {
			afterRenderBody();
		} finally {
			afterRenderEndNs = System.nanoTime();
			statExportNs += afterRenderEndNs - t0;
		}
	}

	private static void afterRenderBody() {
		if (!linked) {
			return;
		}
		// Portalcraft: Minecraft's own frame cost, logged every 5 s (render distance measurements)
		long now = System.nanoTime();
		growthLog(now);
		statWorkNs += now - frameStartNs;
		if (statStartNs == 0) {
			statStartNs = now;
		}
		if (++statFrames >= 1 && now - statStartNs > 5_000_000_000L) {
			Minecraft mc = Minecraft.getInstance();
			String where = "";
			try {
				// (bridge #49) where the client player and the camera are and what its renderer holds (sections drawn / total), for the stage's views
				var hv = dev.skycraft.client.portalcraft.HostViewClient.view();
				var cp = mc.level == null ? null : mc.level.getChunkSource().getChunk(mc.player.getBlockX() >> 4, mc.player.getBlockZ() >> 4, net.minecraft.world.level.chunk.status.ChunkStatus.FULL, false);
				var cc = mc.level == null || hv == null ? null : mc.level.getChunkSource().getChunk((int) Math.floor(hv.x) >> 4, (int) Math.floor(hv.z) >> 4, net.minecraft.world.level.chunk.status.ChunkStatus.FULL, false);
				boolean lit = hv != null && mc.level != null && mc.level.getLightEngine().lightOnInColumn(net.minecraft.world.level.ChunkPos.pack((int) Math.floor(hv.x) >> 4, (int) Math.floor(hv.z) >> 4));
				int litCols = 0, loadedCols = 0;
				if (hv != null && mc.level != null) {
					int ccx = (int) Math.floor(hv.x) >> 4, ccz = (int) Math.floor(hv.z) >> 4;
					for (int ax = -6; ax <= 6; ax++) {
						for (int az = -6; az <= 6; az++) {
							loadedCols += mc.level.getChunkSource().getChunk(ccx + ax, ccz + az, net.minecraft.world.level.chunk.status.ChunkStatus.FULL, false) != null ? 1 : 0;
							litCols += mc.level.getLightEngine().lightOnInColumn(net.minecraft.world.level.ChunkPos.pack(ccx + ax, ccz + az)) ? 1 : 0;
						}
					}
				}
				where = String.format(" [chunks around the camera (13 x 13): %d loaded, %d lit]", loadedCols, litCols) + String.format(", player (%.1f %.1f %.1f) own chunk %s, host camera %s chunk %s lit %s, renderer: expects %s, %d visible sections, %d chunks expected, all rendered %s", mc.player.getX(), mc.player.getY(), mc.player.getZ(), cp == null ? "NOT LOADED" : "loaded",
					hv == null ? "-" : String.format("(%.1f %.1f %.1f)", hv.x, hv.y, hv.z), cc == null ? "NOT LOADED" : "loaded", lit, expectedChunkList(mc), mc.levelRenderer.visibleSections().size(), mc.levelRenderer.sectionOcclusionGraph().expectedChunks().size(), mc.levelRenderer.hasRenderedAllSections());
			} catch (RuntimeException e) {
				where = ", (" + e + ")";
			}
			SkyCraft.LOG.info("Portalcraft: frame stats: {} fps, {} ms work per frame (tick + render, CPU wall), then export {} ms, swap {} ms, pace wait {} ms ({} stalled waits), render distance {}, host view {}{}",
				String.format("%.1f", statFrames * 1e9 / (now - statStartNs)), String.format("%.2f", statWorkNs / 1e6 / statFrames),
				String.format("%.2f", statExportNs / 1e6 / statFrames), String.format("%.2f", statSwapNs / 1e6 / statFrames), String.format("%.2f", statPaceNs / 1e6 / statFrames), statStalls,
				mc.options.getEffectiveRenderDistance(), dev.skycraft.client.portalcraft.HostViewClient.active(), where);
			statStartNs = now;
			statFrames = 0;
			statWorkNs = 0;
			statExportNs = statSwapNs = statPaceNs = 0;
			statStalls = 0;
		}
		Minecraft minecraft = Minecraft.getInstance();
		LocalPlayer player = minecraft.player;
		int flags = 0;
		if (player != null && minecraft.level != null) {
			float partial = minecraft.getDeltaTracker().getGameTimeDeltaPartialTick(false);
			Vec3 feet = player.getPosition(partial);
			Camera camera = minecraft.gameRenderer.mainCamera();
			flags |= Proto.MC_IN_WORLD;
			if (player.onGround()) {
				flags |= Proto.MC_ON_GROUND;
			}
			if (player.isShiftKeyDown()) {
				flags |= Proto.MC_SNEAKING;
			}
			if (player.isSprinting()) {
				flags |= Proto.MC_SPRINTING;
			}
			if (player.isDeadOrDying()) {
				flags |= Proto.MC_DEAD;
			}
			if (player.isSwimming()) {
				flags |= Proto.MC_SWIMMING;
			}
			if (player.getAbilities().flying) {
				flags |= Proto.MC_FLYING;
			}
			mc.x = feet.x;
			mc.y = feet.y;
			mc.z = feet.z;
			mc.yaw = player.getYRot();
			mc.pitch = player.getXRot();
			// The eye, not the camera: in third person Minecraft's camera sits behind or in front.
			// (bridge #14: also while a host view drives the camera, which then sits at the host's camera)
			boolean hostCam = dev.skycraft.client.portalcraft.HostViewClient.active();
			Vec3 eye = camera.isDetached() || hostCam ? player.getEyePosition(partial) : camera.position();
			mc.eyeHeight = (float) (eye.y - feet.y);
			mc.eyeX = eye.x;
			mc.eyeY = eye.y;
			mc.eyeZ = eye.z;
			mc.fov = hostCam ? dev.skycraft.client.portalcraft.HostViewClient.playerFov() : camera.getFov();
			// Minecraft's F5 camera: Skyrim puts its camera where Minecraft's would be.
			mc.cameraMode = minecraft.options.getCameraType().ordinal();
			// (bridge #16: the distance Minecraft's own F5 camera chose, zoom collision included, host view or not)
			mc.cameraDistance = dev.skycraft.client.portalcraft.AvatarHost.zoom(minecraft);
			// Walk bob, exactly what GameRenderer.bobView() uses this frame.
			var entityState = minecraft.gameRenderer.gameRenderState().levelRenderState.cameraRenderState.entityRenderState;
			boolean bob = minecraft.options.bobView().get() && entityState.isPlayer;
			mc.bobPhase = bob ? entityState.backwardsInterpolatedWalkDistance : 0.0F;
			mc.bobAmount = bob ? entityState.bob : 0.0F;
		}
		if (minecraft.gui.screen() != null) {
			flags |= Proto.MC_SCREEN_OPEN;
		}
		mc.flags = flags;
		mc.sensitivity = minecraft.options.sensitivity().get().floatValue();
		// not "arrived" until we are released (bridge #40: and, after a body hand-over, two ticks on: the host takes over Minecraft's position at the ack, so the
		// first thing it reads is a tick Minecraft has carried on the mover, not the held position of the tick before)
		mc.teleportAck = holdPos == null && ackHoldTicks <= 0 ? teleportAck : teleportAck - 1;
		mc.guiScale = minecraft.getWindow().getGuiScale();
		mc.frameCounter = ++frameCounter;
		SkyLink.writeMcState(mc);

		if ((flags & Proto.MC_IN_WORLD) != 0) {
			try {
				WorldExporter.frame(minecraft, minecraft.getDeltaTracker().getGameTimeDeltaPartialTick(false));
			} catch (RuntimeException e) {
				if (exporterErrors++ < 5) {
					SkyCraft.LOG.error("SkyCraft: world export failed", e);
				}
			}
			// Portalcraft: while the host wants Minecraft's own frame, the overlay carries that.
			if (dev.skycraft.client.portalcraft.HostViewClient.active()) {
				dev.skycraft.client.portalcraft.WorldViewExporter.ship();
			} else if (!dev.skycraft.client.portalcraft.HudExporter.wanted()) {
				FrameExporter.capture(minecraft);
			}
			// v17: the Minecraft player's hand + GUI, its own buffer (the overlay may carry a world view)
			dev.skycraft.client.portalcraft.HudExporter.capture(minecraft, flags);
		}
	}

	// (bridge #53) pacing state: when the host's last frame was seen, an average of the host's frame period, when our last frame ended
	private static long paceChangeNs;
	private static double paceHostPeriodNs = 16_000_000.0;
	private static long paceEndNs;

	/**
	 * End of the frame: render at most once per Skyrim (Portal 2) frame instead of spinning freely.
	 * (bridge #53) Three changes: (1) the wait is parked, not spun (a spinning wait kept a core busy for the length of every host
	 * frame); (2) it lasts for the host's own frame period (twice the average, 25 .. 100 ms) instead of a fixed 25 ms: a host that
	 * draws a frame every 33 or 54 ms (unfocused, or a heavy view) timed the old wait out, and Minecraft then ran FREE until the
	 * next host frame, up to 260 fps of world export and GPU work competing with Portal 2 for the same GPU, which kept the host
	 * slow (a loop that outlived the cause); (3) a host that has really stopped (no frame for the whole wait) lets Minecraft run
	 * at about 30 fps, not at the cap.
	 */
	public static void paceFrame() {
		if (!linked) {
			return;
		}
		WinTimer.ensure();  // (Windows: 1 ms sleeps)
		long paceStart = System.nanoTime();
		if (afterRenderEndNs != 0) {
			statSwapNs += paceStart - afterRenderEndNs;
		}
		try {
			paceFrameBody();
		} finally {
			statPaceNs += System.nanoTime() - paceStart;
		}
	}

	private static void paceFrameBody() {
		if (skyrimStalled && (SkyLink.skyStateSeq() >>> 1) == lastPacedSeq) {
			// the host is paused (menu / alt-tab / hung): about 30 frames a second, parked, until it moves again
			++statStalls;
			long wake = paceEndNs + 33_000_000L;
			while ((SkyLink.skyStateSeq() >>> 1) == lastPacedSeq && System.nanoTime() < wake) {
				java.util.concurrent.locks.LockSupport.parkNanos(500_000L);
			}
			paceEndNs = System.nanoTime();
			return;
		}
		skyrimStalled = false;
		long start = System.nanoTime();
		long deadline = start + (long) Math.max(25_000_000.0, Math.min(100_000_000.0, 2.0 * paceHostPeriodNs));
		// SkyState.seq advances by 2 per Skyrim frame (odd while writing).
		while ((SkyLink.skyStateSeq() >>> 1) == lastPacedSeq) {
			long left = deadline - System.nanoTime();
			if (left <= 0) {
				break;
			}
			if (left > 600_000L) {
				java.util.concurrent.locks.LockSupport.parkNanos(Math.min(left - 300_000L, 250_000L));
			} else {
				Thread.onSpinWait();
			}
		}
		long end = System.nanoTime();
		int seqNow = SkyLink.skyStateSeq() >>> 1;
		if (seqNow != lastPacedSeq) {
			if (paceChangeNs != 0) {
				double sample = (double) (end - paceChangeNs) / (seqNow - lastPacedSeq);
				if (sample < 150_000_000.0) { // (a pause is not a frame period)
					paceHostPeriodNs += (sample - paceHostPeriodNs) * 0.1;
				}
			}
			paceChangeNs = end;
		}
		skyrimStalled = seqNow == lastPacedSeq;
		lastPacedSeq = seqNow;
		paceEndNs = end;
	}

	private static long growthLastNs;
	private static long growthGcCount, growthGcMs;

	/** (bridge #53) every 60 s: what could grow with the length of a session, in one line of the log (the host prints its own "growth" line). */
	private static void growthLog(long now) {
		if (growthLastNs == 0) {
			growthLastNs = now;
			return;
		}
		if (now - growthLastNs < 60_000_000_000L) {
			return;
		}
		growthLastNs = now;
		try {
			Runtime rt = Runtime.getRuntime();
			long gcCount = 0, gcMs = 0;
			for (var gc : java.lang.management.ManagementFactory.getGarbageCollectorMXBeans()) {
				gcCount += Math.max(0, gc.getCollectionCount());
				gcMs += Math.max(0, gc.getCollectionTime());
			}
			long directBytes = 0, directCount = 0;
			for (var pool : java.lang.management.ManagementFactory.getPlatformMXBeans(java.lang.management.BufferPoolMXBean.class)) {
				if ("direct".equals(pool.getName())) {
					directBytes = pool.getMemoryUsed();
					directCount = pool.getCount();
				}
			}
			Minecraft mcc = Minecraft.getInstance();
			String level = "";
			if (mcc.level != null) {
				level = ", chunk source " + reflectString(mcc.level, "gatherChunkSourceStats") + ", entities " + reflectString(mcc.level, "getEntityCount")
					+ ", visible sections " + mcc.levelRenderer.visibleSections().size();
			}
			SkyCraft.LOG.info("Portalcraft: growth: heap {} / {} MB, GC {} collections (+{}), {} ms (+{}), direct buffers {} ({} MB), threads {}{} | {} | {} | {} | {} | {} | {} | {}",
				(rt.totalMemory() - rt.freeMemory()) >> 20, rt.maxMemory() >> 20, gcCount, gcCount - growthGcCount, gcMs, gcMs - growthGcMs, directCount, directBytes >> 20,
				Thread.activeCount(), level, dev.skycraft.world.SkyCollision.growth(), dev.skycraft.world.SkyDig.growth(), dev.skycraft.world.SkyMovers.growth(),
				dev.skycraft.portalcraft.HostWorld.growth(), dev.skycraft.client.render.WorldExporter.growth(),
				dev.skycraft.combat.SkyCombat.growth(), dev.skycraft.client.SkyDigClient.growth());
			growthGcCount = gcCount;
			growthGcMs = gcMs;
		} catch (RuntimeException e) {
			SkyCraft.LOG.info("Portalcraft: growth line failed: {}", e.toString());
		}
	}

	/** A no-argument method's result as text, "?" when the class has no such method (this build's Minecraft names). */
	private static String reflectString(Object target, String method) {
		try {
			Object r = target.getClass().getMethod(method).invoke(target);
			return String.valueOf(r);
		} catch (ReflectiveOperationException | RuntimeException e) {
			return "?";
		}
	}

	private static void applyLinkedOptions() {
		Minecraft minecraft = Minecraft.getInstance();
		var options = minecraft.options;
		options.pauseOnLostFocus = false;
		options.vignette().set(false);
		options.enableVsync().set(false);
		// Portalcraft: -Dportalcraft.mcFps caps Minecraft's frames (the host reprojects between them),
		// so Minecraft doesn't compete with Portal 2 for the GPU at the host's full frame rate.
		options.framerateLimit().set(Integer.getInteger("portalcraft.mcFps", 260));
		// Minecraft doesn't draw the world itself; these only decide how far out placed blocks,
		// arrows and Skyrim NPC stand-ins stay loaded and simulated.
		// Portalcraft: terrain mode ships a whole landscape to the host, so it can ask for more.
		int distance = Integer.getInteger("skycraft.renderDistance", 8);
		options.renderDistance().set(distance);
		options.simulationDistance().set(Math.min(distance, 10));
		options.autoJump().set(false);
		options.onboardAccessibility = false;
		if (options.tutorialStep != net.minecraft.client.tutorial.TutorialSteps.NONE) {
			minecraft.getTutorial().setStep(net.minecraft.client.tutorial.TutorialSteps.NONE);
		}
		options.getSoundSourceOptionInstance(net.minecraft.sounds.SoundSource.MUSIC).set(0.0);
		options.save();
	}

	private static void hideWindowOnce(Minecraft minecraft) {
		if (windowHidden || SHOW_WINDOW) {
			return;
		}
		windowHidden = true;
		SDLVideo.SDL_HideWindow(minecraft.getWindow().handle());
		SkyCraft.LOG.info("SkyCraft: game window hidden (run with -Dskycraft.showWindow=true to keep it)");
	}

	private static void applyViewportSize(Minecraft minecraft) {
		int w = Math.min(sky.viewportW, Proto.MAX_OVERLAY_W);
		int h = Math.min(sky.viewportH, Proto.MAX_OVERLAY_H);
		if (w <= 0 || h <= 0 || (w == appliedViewportW && h == appliedViewportH)) {
			return;
		}
		appliedViewportW = w;
		appliedViewportH = h;
		minecraft.getWindow().setWindowed(w, h);
		SkyCraft.LOG.info("SkyCraft: sizing overlay to Skyrim viewport {}x{}", w, h);
	}
}
