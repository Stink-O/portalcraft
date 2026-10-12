// SkyCraft shared-memory protocol (Skyrim SKSE plugin <-> Minecraft Fabric mod).
// Portalcraft fork: the host is a Portal 2 server plugin on Linux (bridge/src/bridge/link.cpp).
// Layouts are unchanged; bridge/tools/layout_test.sh checks this header (built -m32, as the
// plugin is) against fabric/.../Proto.java. Clocks are CLOCK_MONOTONIC (ms for heartbeats, ns
// for the tick timestamps that upstream takes from QueryPerformanceCounter).
//
// This header is the single source of truth for the byte layout. The Java side mirrors it in
// fabric/src/main/java/dev/skycraft/link/Proto.java; if you change anything here, change it
// there too and bump kVersion.
//
// All multi-byte values are little-endian. Skyrim creates the mapping; Minecraft opens it.
// Coordinates in this protocol are always Minecraft space (blocks, Y up, Z south) unless noted.
#pragma once

#include <cstdint>

namespace skycraft::proto
{
	inline constexpr std::uint32_t kMagic = 0x43594B53;  // "SKYC"
	inline constexpr std::uint32_t kVersion = 18;  // 11: Portalcraft HostView + world-view overlay frames; 12: depth grid; 13: HostViewExt (crop, 2nd camera); 14: HostAudio; 15: HostPortals; 16: light probes; 17: HostPlayer + HUD triple buffer; 18: movers (McMovers, kColMoverShape/Bind; the platform messages retired); bridge #28 added McState transitSeq/transitPortals, McTransit @0x310 and SkyState transitAck in spare bytes (same layout, v18)
#if defined(_WIN32)
	inline constexpr wchar_t       kMappingName[] = L"Local\\Portalcraft_v1";
#else
	// Portalcraft (Linux port): POSIX shared memory, i.e. the file /dev/shm/Portalcraft_v1.
	inline constexpr char          kShmName[] = "/Portalcraft_v1";
#endif

	// Portalcraft: 1 Minecraft block == 40 Portal 2 (Source) units: the 72 u player is 1.8 blocks.
	// (SkyCraft upstream: 70 Skyrim units.)
	inline constexpr double kUnitsPerBlock = 40.0;

	// ---- region offsets ---------------------------------------------------------------------
	inline constexpr std::uint64_t kOffHeader = 0x0;
	inline constexpr std::uint64_t kOffHostAudio = 0x80;    // Portalcraft (v14), in the header page's spare space
	inline constexpr std::uint64_t kOffSkyState = 0x100;
	inline constexpr std::uint64_t kOffHostView = 0x140;  // Portalcraft (v11), after SkyState
	inline constexpr std::uint64_t kOffHostViewExt = 0x188;  // Portalcraft (v13), after HostView
	inline constexpr std::uint64_t kOffMcState = 0x200;
	inline constexpr std::uint64_t kOffOverlayCtl = 0x300;
	inline constexpr std::uint64_t kOffOverlaySlotHdr = 0x340;  // 3 x 0x40
	inline constexpr std::uint64_t kOffWaterGrid = 0x400;       // Skyrim -> MC, see WaterGrid
	inline constexpr std::uint64_t kOffHostPortals = 0xE00;     // Portalcraft (v15), after the water grid's space
	inline constexpr std::uint64_t kOffInputRing = 0x1000;
	inline constexpr std::uint64_t kOffCollisionRing = 0x20000;
	inline constexpr std::uint64_t kCollisionRingBytes = 32ull << 20;
	inline constexpr std::uint64_t kOffOverlayPixels = kOffCollisionRing + kCollisionRingBytes;
	inline constexpr std::uint32_t kMaxOverlayW = 3840;
	inline constexpr std::uint32_t kMaxOverlayH = 2160;
	inline constexpr std::uint64_t kOverlaySlotBytes = std::uint64_t(kMaxOverlayW) * kMaxOverlayH * 4;
	inline constexpr std::uint32_t kOverlaySlots = 3;
	inline constexpr std::uint64_t kOffActorTable = 0x12000;   // Skyrim -> MC, see ActorTable
	inline constexpr std::uint64_t kOffEventRing = 0x17000;    // MC -> Skyrim, see McEvent
	inline constexpr std::uint64_t kOffWorldEntities = 0x1C000;  // MC -> Skyrim, see WorldEntities
	inline constexpr std::uint64_t kOffRenderRing = kOffOverlayPixels + kOverlaySlotBytes * kOverlaySlots;
	inline constexpr std::uint64_t kRenderRingBytes = 64ull << 20;
	// Portalcraft v17: the Minecraft player's HUD image (hand + held item + GUI on transparent), its own
	// triple buffer after the render ring (HudCtl). A v17 mod maps a v16 host's smaller file too.
	inline constexpr std::uint32_t kMaxHudW = 2048;
	inline constexpr std::uint32_t kMaxHudH = 1152;
	inline constexpr std::uint64_t kHudSlotBytes = std::uint64_t(kMaxHudW) * kMaxHudH * 4;
	inline constexpr std::uint32_t kHudSlots = 3;
	inline constexpr std::uint64_t kOffHudPixels = kOffRenderRing + kRenderRingBytes;
	inline constexpr std::uint64_t kMappingBytes = kOffHudPixels + kHudSlotBytes * kHudSlots;

	// ---- header @0x0 ------------------------------------------------------------------------
	struct Header
	{
		std::uint32_t magic;
		std::uint32_t version;
		std::uint32_t skyrimPid;
		std::uint32_t mcPid;
		std::uint64_t skyrimHeartbeatMs;  // GetTickCount64() at last Skyrim frame
		std::uint64_t mcHeartbeatMs;      // GetTickCount64() at last MC frame
	};
	static_assert(sizeof(Header) == 0x20);

	// ---- Portalcraft: Minecraft's audio listener @0x80 (seqlock, Host -> MC, v14) --------------
	// Minecraft hears from where the host's player hears it: Chell's head mapped into Minecraft space
	// (in the region), or carried through the portal pair she is near; never from the hidden player.
	// gain is Minecraft's overall level (0 = silent), already smoothed and scaled by the host.
	enum HostAudioFlags : std::uint32_t
	{
		kAudioActive = 1u << 0,
		kAudioChamber = 1u << 1,    // bridge #23: the listener is in a Portal 2 chamber: only local sounds (blocks, players, mobs,
		                            //   voice); weather, ambient, music and records are muted (the chamber band is in Minecraft's open sky)
		kAudioLogSounds = 1u << 2,  // bridge #23 (tests): the mod logs each sound it starts, with its category and the host's level
	};

	struct HostAudio
	{
		std::uint32_t seq;
		std::uint32_t flags;    // HostAudioFlags
		double        x, y, z;  // listener (MC coords)
		float         yaw, pitch;  // MC degrees
		float         gain;     // 0..1
		float         level;    // bridge #23: the host's level before a test mute (gain is 0 in muted test runs)
	};
	static_assert(sizeof(HostAudio) == 0x30);
	static_assert(kOffHostAudio + sizeof(HostAudio) <= kOffSkyState);

	// ---- Skyrim -> MC state @0x100 (seqlock: seq odd while writing) -------------------------
	enum SkyFlags : std::uint32_t
	{
		kSkyInGame = 1u << 0,    // a save is loaded and the player exists
		kSkyMenuOpen = 1u << 1,  // a Skyrim menu owns input; MC should drop held keys
		kSkyLoading = 1u << 2,   // loading screen / cell transition in progress
	};

	// Skyrim's water (lakes, rivers, the sea) around the player, for Minecraft to treat as its own
	// water: swimming, floating, drowning. Seqlock like SkyState.
	inline constexpr std::uint32_t kWaterGridSize = 16;
	inline constexpr float         kNoWater = -1.0e30f;

	struct WaterGrid
	{
		std::uint32_t seq;
		std::int32_t  originX, originZ;  // Minecraft block column of surface[0]
		std::uint32_t worldId;           // as in SkyState
		float         surface[kWaterGridSize * kWaterGridSize];  // [z * size + x]: MC y of the water surface; kNoWater: none
	};
	static_assert(sizeof(WaterGrid) <= 0xA00);
	static_assert(kOffWaterGrid + 0xA00 <= kOffHostPortals);

	struct SkyState
	{
		std::uint32_t seq;
		std::uint32_t flags;           // SkyFlags
		std::uint32_t worldId;         // worldspace FormID (exterior) or cell FormID (interior)
		std::uint32_t transitAck;      // Portalcraft (bridge #28; was collisionEpoch, never used): yaw/pitch already include the turn of every
		                               // Minecraft player transit up to this McState::transitSeq (the mod turns the host's look itself until then)
		double        posX, posY, posZ;  // Skyrim player feet, MC coords
		float         yaw, pitch;        // authoritative look (MC degrees)
		std::uint32_t teleportSeq;       // MC teleports its player to pos when this changes
		std::uint32_t viewportW, viewportH;
		float         gameHour;
	};
	static_assert(sizeof(SkyState) == 0x40);

	// ---- Portalcraft: host camera for Minecraft's own frame @0x140 (seqlock, Host -> MC) -------
	// While kViewActive is set, Minecraft renders its level (sky, terrain, entities, fog; no hand
	// or HUD) from this camera instead of its player's, and publishes the frame through the
	// overlay triple buffer with kSlotWorldView (colour, then float depth at depthOffset).
	// Used to composite Minecraft into Portal 2: a portal view that looks into the Minecraft
	// region, or the whole screen once the player is in there.
	enum HostViewFlags : std::uint32_t
	{
		kViewActive = 1u << 0,
		kViewClipPlane = 1u << 1,   // plane[] is an oblique near plane (a portal's surface)
		kViewFullScreen = 1u << 2,  // the host's main view (else a portal view)
		kViewScreenshot = 1u << 3,  // also save this frame as a PNG (verification)
	};

	struct HostView
	{
		std::uint32_t seq;
		std::uint32_t flags;          // HostViewFlags
		double        x, y, z;        // camera position (MC coords)
		float         yaw, pitch, roll;  // MC degrees (roll about the view axis, rotationYXZ's Z)
		float         fovY;           // vertical FOV, degrees
		float         plane[4];       // MC world plane n.p + d, kept where >= 0 (n unit length)
		std::uint32_t requestId;      // echoed in the frame's slot header
		std::uint32_t pad;
	};
	static_assert(sizeof(HostView) == 0x48);
	static_assert(kOffHostView + sizeof(HostView) <= kOffHostViewExt);

	// ---- Portalcraft v13: crop windows and a second camera @0x188 (seqlock, Host -> MC) -------
	// Written by v13 hosts just before every HostView; read only when the header's version is 13
	// and primaryRequestId equals the HostView's requestId (else ignored: v11/v12 behaviour).
	// Crop: Minecraft renders only the NDC window [x0,x1] x [y0,y1] of the request's frustum, at
	// one pixel per pixel of the full frame, into the bottom-left corner of its target (a pure NDC
	// translation of the projection), and ships just that sub-rectangle: ceil((x1-x0)/2 * W) x
	// ceil((y1-y0)/2 * H) pixels. The second camera (kViewActive in secFlags) is rendered in the
	// same Minecraft frame, after the first, with the first camera's section graph and view area,
	// culled to secRenderDistance chunks, and shipped in the same overlay slot (OverlaySlotHdr img2).
	enum HostViewExtFlags : std::uint32_t
	{
		kExtValid = 1u << 0,
	};

	struct HostViewExt
	{
		std::uint32_t seq;
		std::uint32_t flags;             // HostViewExtFlags
		float         crop[4];           // primary camera: x0, y0, x1, y1 (NDC, -1..1)
		std::uint32_t primaryRequestId;  // the HostView this block belongs to
		std::uint32_t primaryStream;     // host's stream id (diagnostics)
		double        sx, sy, sz;        // second camera (MC coords)
		float         syaw, spitch, sroll, sfovY;
		float         splane[4];
		float         scrop[4];
		std::uint32_t secRequestId;
		std::uint32_t secStream;
		std::uint32_t secFlags;          // HostViewFlags (kViewActive, kViewClipPlane)
		std::uint32_t secRenderDistance; // chunks; 0 = Minecraft's own
	};
	static_assert(sizeof(HostViewExt) == 0x78);
	static_assert(kOffHostViewExt + sizeof(HostViewExt) <= kOffMcState);

	// ---- Portalcraft v15: the live portal pair, drawn by Minecraft itself @0xE00 (seqlock, Host -> MC)
	// Written by v15 hosts just before every HostViewExt/HostView; read only when primaryRequestId
	// equals the HostView's requestId. With kPortalsInMc set, both portals of the live pair are in
	// the Minecraft region: in the primary camera's pass Minecraft renders the view through each
	// visible portal (virtual camera = primary x portal transform, oblique near plane on the
	// partner) into its own target and composites it through the opening (an ellipse of
	// halfW x halfH blocks in the portal's plane, depth-tested), recursing to maxDepth; past that
	// the opening is the portal's flat colour. The depth in alpha inside an opening is the virtual
	// camera's. The host then draws that one image in every region view.
	enum HostPortalsFlags : std::uint32_t
	{
		kPortalsInMc = 1u << 0,
	};

	struct HostPortal
	{
		double        cx, cy, cz;        // opening centre (MC coords)
		float         nx, ny, nz;        // normal, out of the surface (unit)
		float         ux, uy, uz;        // the opening's long axis (unit, in the plane)
		float         halfW, halfH;      // opening half-extents in blocks, at the current open scale
		float         open;              // Portal 2's open amount 0..1
		std::uint32_t colour;            // flat colour past the recursion limit: 0xRRGGBB (display)
	};
	static_assert(sizeof(HostPortal) == 0x40);

	struct HostPortals
	{
		std::uint32_t seq;
		std::uint32_t flags;             // HostPortalsFlags
		std::uint32_t primaryRequestId;  // the HostView this block belongs to
		std::uint32_t maxDepth;          // recursion depth (Portal 2: 2)
		std::uint32_t renderDistance;    // chunks for portal passes; 0 = Minecraft's own
		float         resScale;          // portal pass target scale (1 = the primary's size)
		std::uint32_t pad[2];
		HostPortal    portal[2];
	};
	static_assert(sizeof(HostPortals) == 0xA0);
	static_assert(kOffHostPortals + sizeof(HostPortals) <= kOffInputRing);

	// ---- MC -> Skyrim state @0x200 (seqlock) ------------------------------------------------
	enum McFlags : std::uint32_t
	{
		kMcInWorld = 1u << 0,
		kMcScreenOpen = 1u << 1,  // an MC GUI screen (inventory, chat, ...) is open
		kMcOnGround = 1u << 2,
		kMcSneaking = 1u << 3,
		kMcSprinting = 1u << 4,
		kMcDead = 1u << 5,
		kMcSwimming = 1u << 6,
		kMcFlying = 1u << 7,
	};

	struct McState
	{
		std::uint32_t seq;
		std::uint32_t flags;          // McFlags
		double        x, y, z;        // interpolated feet position (MC coords)
		float         yaw, pitch;     // MC rotation (degrees)
		float         eyeHeight;      // blocks above feet
		float         sensitivity;    // MC mouse sensitivity option (0..1)
		std::uint32_t teleportAck;    // last SkyState::teleportSeq applied
		std::uint32_t guiScale;
		std::uint64_t frameCounter;
		float         fovDeg;         // effective vertical FOV (includes sprint / fluid modifiers)
		float         bobPhase;       // MC walk-bob phase (interpolated walk distance); 0 if bobbing is off
		float         bobAmount;      // MC walk-bob amplitude
		std::uint32_t transitSeq;     // Portalcraft (bridge #28; was pad): bumps when Minecraft transits its player through the portal pair (McTransit)
		double        eyeX, eyeY, eyeZ;  // MC camera position (interpolated, includes sneak eye lerp)

		// Raw 20 Hz physics ticks, so Skyrim can interpolate on its own frame clock exactly like
		// Minecraft's renderer does with partial ticks (no judder from the two games' frame phase).
		std::int64_t tickQpc;             // QueryPerformanceCounter at the (remainder-corrected) tick
		double       prevX, prevY, prevZ;  // feet at the previous tick
		double       curX, curY, curZ;     // feet at the latest tick
		float        tickEyeO, tickEye;      // Camera's smoothed eye height, previous/latest tick
		float        walkDistO, walkDist;    // walk-bob phase inputs
		float        bobO, bob;              // walk-bob amplitude inputs
		float        tickMs;                 // milliseconds per tick (50 unless /tick rate changed)
		std::uint32_t transitPortals; // bridge #28 (was pad): the latest transit's portals, entered | exit << 8

		// Minecraft's camera (F5): 0 first person, 1 third person behind, 2 third person in front
		// (looking back at the player). cameraDistance is how far Minecraft's camera sits from
		// the eye, after its own zoom collision (Minecraft blocks and Skyrim's triangles).
		std::uint32_t cameraMode;
		float         cameraDistance;

		// Portalcraft (v18, bridge #26): the mover (McMovers) the player stood on for the latest tick and was carried by.
		// rideMover = its id (0: none); rideFlags: kRideMoved (it moved this tick). ridePrev/rideCur = the mover's origin
		// (MC coords) as the previous / latest tick was simulated with, so the host interpolates the feet relative to the
		// mover, as the mover is now (replaces v17's platform fields, same bytes).
		std::uint32_t rideMover;
		std::uint32_t rideFlags;
		double        ridePrevX, ridePrevY, ridePrevZ;
		double        rideCurX, rideCurY, rideCurZ;
	};
	static_assert(sizeof(McState) == 0x100);

	// Portalcraft (bridge #28): the latest Minecraft player transit @0x310 (the spare bytes after OverlayCtl), written by MC under
	// McState's seqlock (read it inside the same seq check). The pair transform in MC coordinates: a point p goes to R p + off, a
	// direction d to R d, R = the unit quaternion q (x, y, z, w). frac = the part of the tick before the eye crossed the plane.
	inline constexpr std::uint64_t kOffMcTransit = 0x310;
	struct McTransit
	{
		float         q[4];
		double        off[3];
		float         frac;
		std::uint32_t seq;  // = McState::transitSeq of this transit
	};
	static_assert(sizeof(McTransit) == 0x30);
	static_assert(sizeof(McState) <= 0x100);
	enum McRideFlags : std::uint32_t
	{
		kRideMoved = 1u << 0,   // the ride mover moved between the previous tick and this one (the player was carried)
		kRidePushed = 1u << 1,  // a mover pushed the player this tick (bridge #26 diagnostics)
	};

	// ---- overlay triple buffer @0x300 --------------------------------------------------------
	// state: bits 0-1 = index of the "middle" slot, bit 2 = middle holds an unread frame.
	// Writer (MC) renders into its private back slot, then xchg(state, back | kDirty) and keeps
	// the returned index as its new back slot. Reader (Skyrim) does xchg(state, front) only when
	// the dirty bit is set and keeps the returned index as its new front slot.
	inline constexpr std::uint32_t kOverlayDirty = 1u << 2;

	struct OverlayCtl
	{
		std::uint32_t state;
		std::uint32_t pad;
		std::uint64_t framesPublished;
	};
	static_assert(kOffOverlayCtl + sizeof(OverlayCtl) <= kOffMcTransit && kOffMcTransit + sizeof(McTransit) <= kOffOverlaySlotHdr);

	inline constexpr std::uint32_t kSlotBottomUp = 1u << 0;
	inline constexpr std::uint32_t kSlotWorldView = 1u << 1;  // Portalcraft: a HostView frame
	// Portalcraft v12: the colour's alpha is per-pixel depth, log-encoded: 255 at kDepthAlphaMin
	// blocks, 1 at kDepthAlphaMax, 0 = nothing drawn (sky); linear depth along the view axis.
	inline constexpr std::uint32_t kSlotDepthAlpha = 1u << 2;
	inline constexpr float kDepthAlphaMin = 0.25f, kDepthAlphaMax = 512.0f;

	struct OverlaySlotHdr
	{
		std::uint32_t width;
		std::uint32_t height;
		std::uint32_t flags;  // kSlotBottomUp, kSlotWorldView
		std::uint32_t pad;
		std::uint64_t frameId;
		// Portalcraft (kSlotWorldView frames): which HostView request this frame answers, and the
		// depth buffer (float32 per pixel, same row order as the colour) at depthOffset bytes
		// from the start of the slot's pixels (0 = none). Minecraft's projection is reversed-Z:
		// depth 1 at zNear, 0 at zFar (blocks), before the oblique near plane.
		std::uint32_t viewRequestId;
		std::uint32_t depthOffset;
		float         zNear, zFar;
		// v12: the depth at depthOffset is a grid of depthW x depthH float32 samples (rows in the
		// colour's row order), sample (i, j) at pixel (i * (width - 1) / (depthW - 1), j * ...),
		// linear depth along the view axis in blocks, unprojected with the frame's own projection
		// (oblique near plane included); 0 = sky / nothing.
		std::uint16_t depthW, depthH;
		// v13: a second image in the same slot (the HostViewExt second camera): its pixels at
		// img2Offset bytes from the slot's pixels (0 = none), rows bottom-up like the first.
		std::uint32_t img2Offset;
		std::uint16_t img2W, img2H;
		std::uint32_t img2RequestId;
		std::uint32_t img2Flags;  // kSlotBottomUp | kSlotWorldView | kSlotDepthAlpha
		std::uint8_t  reserved[0x40 - 0x3C];
	};
	static_assert(sizeof(OverlaySlotHdr) == 0x40);

	// ---- input ring @0x1000 (Skyrim produces, MC consumes) ----------------------------------
	inline constexpr std::uint32_t kInputRingEntries = 4096;  // power of two
	inline constexpr std::uint64_t kInputRingHeadOff = 0x00;  // u64, written by Skyrim
	inline constexpr std::uint64_t kInputRingTailOff = 0x40;  // u64, written by MC
	inline constexpr std::uint64_t kInputRingDataOff = 0x80;

	enum InputType : std::uint16_t
	{
		kInKey = 1,          // code = SDL scancode, a = 1 press / 0 release
		kInMouseButton = 2,  // code = SDL button (1 L, 2 M, 3 R, 4 X1, 5 X2), a = 1 press / 0 release
		kInScroll = 3,       // a = wheel notches * 120 (positive = up)
		kInCursor = 4,       // a, b = absolute cursor position in overlay pixels
		kInText = 5,         // a = unicode code point
		kInReleaseAll = 6,   // release every held key/button (input focus left MC)
		kInHurt = 7,         // Skyrim hit the player: code = HurtKind, a = Skyrim damage * 100, b = attacker FormID, c = HurtFlags
		kInOpenMenu = 8,     // open Minecraft's pause/options menu
		// Portalcraft (bridge #13, Minecraft-player mode; older mods ignore unknown types):
		kInVelocity = 9,     // a, b, c = the player's velocity in MC blocks/tick x 10000, applied when the pending teleport's hold releases (or now)
		kInHole = 10,        // a portal hole carved out of Minecraft's block collision for the player: code = portal (0/1) | 2 for the max corner | 4 = clear;
		                     //   a, b, c = MC coords x 1000 (min corner first, then max; the max event makes the hole live)
		kInHostLight = 11,   // bridge #19: Portal 2's light on a chamber cell: code = light level 0-15 (0x100 = clear all), a, b, c = MC block
		kInHazard = 12,      // bridge #19: a Portal 2 hazard box for Minecraft's mobs: code = kind (1 lethal, 2 laser segment, 3 fizzler,
		                     //   4 hurt) | 0x10 max corner (live) | 0x20 clear all | slot << 8; a, b, c = MC coords x 1000 (min, then max)
		kInPortal = 13,      // bridge #22: the linked portal pair, for Minecraft's mobs and items to go through: code = portal (0/1) | part << 1
		                     //   (0 centre: MC x 1000; 1 normal out of the surface x 1000000; 2 up axis x 1000000, makes the pose live) | 0x10 clear both
		// 14, 15: bridge #23/#25's kInPlatform / kInPlatformRot, retired in v18 (the movers' poses are the McMovers block)
	};

	enum HurtKind : std::uint16_t
	{
		kHurtMelee = 0,
		kHurtProjectile = 1,
		kHurtMagic = 2,
		kHurtOther = 3,
	};

	enum HurtFlags : std::uint32_t
	{
		kHurtBlockedInSkyrim = 1u << 0,
		kHurtPowerAttack = 1u << 1,
	};

	// ---- actor table @0x12000 (Skyrim -> MC, seqlock) ----------------------------------------
	// Nearby Skyrim actors, mirrored in Minecraft as invisible hittable proxy entities.
	inline constexpr std::uint32_t kMaxActors = 256;

	enum ActorFlags : std::uint32_t
	{
		kActorHostile = 1u << 0,    // hostile to the player right now
		kActorDead = 1u << 1,
		kActorEssential = 1u << 2,
		kActorInCombat = 1u << 3,
	};

	struct ActorRecord
	{
		std::uint32_t formId;
		std::uint32_t flags;       // ActorFlags
		float         x, y, z;     // feet, MC coords
		float         yaw;         // MC degrees
		float         width;       // blocks
		float         height;      // blocks
		float         healthFrac;  // 0..1
		std::uint16_t level;
		std::uint16_t pad;
		char          name[24];    // display name, UTF-8, NUL-terminated (truncated)
	};
	static_assert(sizeof(ActorRecord) == 64);

	struct ActorTable
	{
		std::uint32_t seq;
		std::uint32_t count;
		std::uint8_t  pad[0x40 - 8];
		ActorRecord   actors[kMaxActors];
	};
	static_assert(sizeof(ActorTable) == 0x40 + 64 * kMaxActors);

	// ---- Portalcraft v16: Minecraft's light on Portal 2 objects in the region --------------------
	// The host asks for Minecraft's light at up to kMaxLightProbes points (Chell's eye, her body,
	// cubes and props in the region; MC coords) and lights those objects with the answer instead of
	// Portal 2's baked lighting. Both blocks are seqlocks; the answer echoes the request id and keeps
	// the probe order. Both live in the gap after the actor table.
	inline constexpr std::uint32_t kMaxLightProbes = 16;
	inline constexpr std::uint64_t kOffLightProbes = 0x16100;  // Host -> MC
	inline constexpr std::uint64_t kOffMcLight = 0x16400;      // MC -> Host

	struct LightProbes
	{
		std::uint32_t seq;
		std::uint32_t count;      // probes in use (<= kMaxLightProbes)
		std::uint32_t requestId;  // echoed in McLight
		std::uint32_t pad;
		double        pos[kMaxLightProbes][3];  // MC coords
	};
	static_assert(sizeof(LightProbes) == 0x190);

	struct LightSample
	{
		float r, g, b;     // Minecraft's lightmap colour at the probe's block (0..1, display values:
		                   // what Minecraft multiplies an entity's or held item's texture by)
		float sky, block;  // raw light levels at that block, 0..15
		float skyVisible;  // 1 when the block sees the sky straight up (no roof), else 0
		float pad[2];
	};
	static_assert(sizeof(LightSample) == 0x20);

	struct McLight
	{
		std::uint32_t seq;
		std::uint32_t requestId;  // the LightProbes request this answers
		std::uint32_t count;
		std::uint32_t flags;      // reserved
		float         sunDir[3];  // unit vector towards the sun (MC coords); the moon is opposite
		float         daylight;   // Minecraft's sky light factor 0..1 (time of day, weather)
		float         skyColor[3];  // the sky's colour (0..1)
		float         dayTime;      // ticks into the day, 0..24000 (6000 = noon, 18000 = midnight)
		float         skyLightColor[3];  // the lightmap's sky light colour
		float         pad;
		LightSample   s[kMaxLightProbes];
	};
	static_assert(sizeof(McLight) == 0x240);
	static_assert(kOffActorTable + sizeof(ActorTable) <= kOffLightProbes);
	static_assert(kOffLightProbes + sizeof(LightProbes) <= kOffMcLight);
	static_assert(kOffMcLight + sizeof(McLight) <= kOffEventRing);

	// ---- Portalcraft v17: the Minecraft player (DESIGN-mcplayer M2) ---------------------------
	// HostPlayer @0x16800 (seqlock, Host -> MC). With kHpWantHud set, Minecraft clears its main target to
	// transparent after the level (and any host-view capture), so its own first-person hand, held item and
	// GUI (hotbar, health, hunger, XP, crosshair, chat, every screen) land on a transparent background, and
	// ships that image every frame through the HUD triple buffer (HudCtl, same protocol as OverlayCtl).
	enum HostPlayerFlags : std::uint32_t
	{
		kHpMcMode = 1u << 0,    // the Minecraft player is the player
		kHpWantHud = 1u << 1,   // ship the HUD image
		kHpWantAvatar = 1u << 2,  // bridge #16: ship kRenAvatar in first person too (the host draws the body in portal views)
		kHpAvatarInMc = 1u << 3,  // bridge #16: the body is in the region: Minecraft draws its player in host-view passes
		kHpMcTransit = 1u << 4,   // bridge #28: Minecraft transits its own player through the portal pair (PlayerTransit); without it the mod never does
		kHpDig = 1u << 6,         // bridge #33: the host understands digging (kRenDug, ChamberMaterial) and leaves dug cells out of its collision and picture; without it the mod never digs the host's surfaces
		kHpStage = 1u << 7,       // bridge #47: the host wants the Minecraft stage (real terrain, high above the chambers, for the portal onto Minecraft in every campaign chamber) made around chunk stageChunkX/Z, radius stageRadius
		kHpHandOver = 1u << 5,    // bridge #31: the body is being handed to Minecraft and the movers around it are not in yet: a held (teleported) player stays put on the body, however much static ground is below
	};

	struct HostPlayer
	{
		std::uint32_t seq;
		std::uint32_t flags;     // HostPlayerFlags
		std::uint32_t screenW, screenH;  // the host's back buffer
		std::int32_t  cursorX, cursorY;  // the host's cursor in HUD pixels (screens), top-left origin
		float         handLight[3];  // bridge #27: Portal 2's light at the Minecraft player's hand (linear, x the tonemap scale), before the roof term
		std::uint32_t handLightSeq;  // 0 = not set (Minecraft lights its hand itself); bumps when handLight is written
		float         handTone;      // bridge #27: the host's tonemap scale (Minecraft's block-light term gets it too, as placed blocks do)
		std::uint32_t digMaterial;   // bridge #33 (with kHpDig): what the host surface under the crosshair is made of: low byte ChamberMaterial, bit 8 = portalable
		std::uint32_t digWorld;      // bridge #33: this map's dig world id (a hash of the map name; dug cells are kept per world)
		std::int32_t  stageChunkX;   // bridge #47 (was pad[0]; with kHpStage): the stage's centre chunk
		std::int32_t  stageChunkZ;   // bridge #47 (was pad[1])
		std::uint32_t stageRadius;   // bridge #47 (was pad[2]): chunks around the centre that get the stage (0 = the mod's default, 8)
	};
	static_assert(sizeof(HostPlayer) == 0x40);
	inline constexpr std::uint64_t kOffHostPlayer = 0x16800;

	struct HudSlotHdr
	{
		std::uint32_t width;
		std::uint32_t height;
		std::uint32_t flags;     // kSlotBottomUp
		std::uint32_t guiScale;  // Minecraft's GUI scale for this frame
		std::uint64_t frameId;
		std::uint32_t mcFlags;   // McFlags when the frame was rendered (kMcScreenOpen)
		std::uint32_t cameraMode;  // 0 first person (Minecraft drew its crosshair), 1/2 third person
	};
	static_assert(sizeof(HudSlotHdr) == 0x20);

	struct HudCtl
	{
		std::uint32_t state;     // as OverlayCtl::state (bits 0-1 middle slot, kOverlayDirty)
		std::uint32_t pad;
		std::uint64_t framesPublished;
		std::uint8_t  reserved[0x30];
		HudSlotHdr    slot[kHudSlots];
	};
	static_assert(sizeof(HudCtl) == 0xA0);
	inline constexpr std::uint64_t kOffHudCtl = 0x16900;
	static_assert(kOffMcLight + sizeof(McLight) <= kOffHostPlayer);
	static_assert(kOffHostPlayer + sizeof(HostPlayer) <= kOffHudCtl);
	static_assert(kOffHudCtl + sizeof(HudCtl) <= kOffEventRing);

	// ---- event ring @0x17000 (MC -> Skyrim) ---------------------------------------------------
	inline constexpr std::uint32_t kEventRingEntries = 512;  // power of two
	inline constexpr std::uint64_t kEventRingHeadOff = 0x00;  // u64, written by MC
	inline constexpr std::uint64_t kEventRingTailOff = 0x40;  // u64, written by Skyrim
	inline constexpr std::uint64_t kEventRingDataOff = 0x80;

	enum McEventType : std::uint32_t
	{
		kEvHitActor = 1,    // formId, a = MC damage (after MC's own modifiers), b/c = knockback dir x/z (MC), d = knockback strength
		kEvPlayerDied = 2,  // the Minecraft player died: kill the Skyrim player
		kEvExplosion = 3,   // a Minecraft explosion (TNT, creeper, ...): a/b/c = centre (MC coords), d = radius (blocks)
		kEvArrowStuck = 4,  // an arrow stuck in a Skyrim actor: formId, a/b/c = where it hit (MC coords), d = flight yaw,
		                    // flags = flight pitch (float bits), weapon = arrow texture (0 plain, 1 tipped, 2 spectral)
		kEvSkillUse = 5,    // the player used a Skyrim skill in Minecraft: formId = Skyrim skill (ActorValue: 9 Block,
		                    // 10 Smithing, 11 Heavy Armor, 12 Light Armor), a = uses (as Skyrim's AdvanceSkill counts them)
	};

	enum HitFlags : std::uint32_t
	{
		kHitCritical = 1u << 0,
		kHitProjectile = 1u << 1,
		kHitSweep = 1u << 2,
		kHitFire = 1u << 3,
	};

	// What landed a kEvHitActor (Skyrim plays that weapon class's impact effect and sounds).
	enum HitWeapon : std::uint32_t
	{
		kWeaponUnarmed = 0,
		kWeaponBlade = 1,   // swords
		kWeaponAxe = 2,
		kWeaponBlunt = 3,   // maces, pickaxes, shovels, hoes, anything else held
		kWeaponPierce = 4,  // tridents, spears
		kWeaponArrow = 5,   // arrows and other projectiles
	};

	struct McEvent
	{
		std::uint32_t type;
		std::uint32_t formId;
		float         a, b, c, d;
		std::uint32_t flags;
		std::uint32_t weapon;  // HitWeapon for kEvHitActor
	};
	static_assert(sizeof(McEvent) == 32);

	// ---- world entities @0x1C000 (MC -> Skyrim, seqlock) -------------------------------------
	// Minecraft things Skyrim draws itself each frame (arrows, dropped items, block cracks) + the
	// block outline.
	inline constexpr std::uint32_t kMaxWorldEntities = 160;

	enum WorldEntityKind : std::uint32_t
	{
		kWeArrow = 1,    // uv[0]: the arrow's item icon
		kWeItem = 2,     // dropped/thrown item: a flat sprite (uv[0]) turning about the vertical
		kWeTrident = 3,  // uv[0]: the trident's item icon
		kWeBlock = 4,    // dropped block item: a spinning cube of side `scale`, uv[0..2] = side, top, bottom
		kWeCrack = 5,    // block-breaking cracks over the box at (x, y, z) of size ext, uv[0] = crack stage
		kWeShadow = 6,   // a player's or mob's feet at (x, y, z), `scale` wide: its soft contact shadow
	};

	struct WorldEntity
	{
		std::uint32_t kind;        // WorldEntityKind
		std::uint32_t id;          // MC entity id (stable while it exists)
		float         x, y, z;     // MC coords (interpolated at MC's render time)
		float         yaw, pitch;  // MC degrees
		float         scale;
		float         ext[3];      // kWeCrack: box size
		float         uv[3][4];    // atlas rects {u0, v0, u1, v1}
		std::uint32_t tint;        // RGBA8 multiplier for the top face (grass, leaves); 0 = none
	};
	static_assert(sizeof(WorldEntity) == 96);

	struct WorldEntities
	{
		std::uint32_t seq;
		std::uint32_t count;
		std::uint32_t hasSelection;           // draw an outline around the targeted block
		float         selMin[3], selMax[3];   // MC coords
		std::uint8_t  pad[0x40 - 36];
		WorldEntity   entities[kMaxWorldEntities];
	};
	static_assert(sizeof(WorldEntities) == 0x40 + sizeof(WorldEntity) * kMaxWorldEntities);
	static_assert(kOffWorldEntities + sizeof(WorldEntities) <= kOffCollisionRing);

	// ---- render ring (MC -> Skyrim) -----------------------------------------------------------
	// Byte ring like the collision ring. Minecraft ships its own block meshes (built by Minecraft's
	// block renderer: models, tint, AO, lighting) and its block atlas; Skyrim draws them in its own
	// frame so blocks stay locked to the world and are occluded by Skyrim geometry.
	inline constexpr std::uint64_t kRenRingHeadOff = 0x00;
	inline constexpr std::uint64_t kRenRingTailOff = 0x40;
	inline constexpr std::uint64_t kRenRingDataOff = 0x80;
	inline constexpr std::uint64_t kRenRingDataBytes = kRenderRingBytes - kRenRingDataOff;

	enum RenType : std::uint32_t
	{
		kRenPad = 0,
		kRenAtlas = 1,     // RenAtlas + RGBA8 pixels (w * h * 4), top row first
		kRenSection = 2,   // RenSection + RenVertex[vertexCount] (triangle list); 0 vertices = remove
		kRenClearAll = 3,  // drop every section (world change)
		kRenTexture = 4,   // RenTexture + RGBA8 pixels: an entity texture (player skin, armour, ...)
		kRenAvatar = 5,    // RenAvatar + RenBatch[batchCount] + RenVertex[vertexCount]: the player's
		                   // model this frame; 0 batches = not shown (first person)
		kRenScene = 6,     // RenScene + RenBatch[batchCount] + RenVertex[vertexCount]: every other
		                   // entity and all particles this frame, relative to RenScene's origin
		kRenAtlasRegion = 7,  // RenAtlasRegion + RGBA8 pixels: an animated sprite's current frame
		kRenLights = 8,       // RenLights + RenLight[count]: a section's light-emitting blocks (sent
		                      // after its kRenSection; 0 = none)
		kRenDug = 11,         // bridge #33 (SkyCraft's kRenDug): RenDug + 512-byte bitset (bit x + 16z + 256y): which blocks of
		                      // a section the Minecraft player dug out of the host's chamber geometry (its collision and picture there are
		                      // gone); count 0 = none, and no bitset follows. Sent only while the host sets kHpDig.
		kRenMoverDig = 12,    // bridge #43: RenMoverDig: the Minecraft player mined a cell of a mover (a door, a panel, a lift...): mover-local lattice cell (cx, cy, cz)
		                      // of the shape's dig lattice (ColMoverLattice); the host keeps the cell out of that mover's collision and picture from now on
		kRenSolids = 10,      // RenSolids + 512-byte bitset (bit x + 16z + 256y): which blocks of a
		                      // section NPCs collide with (sent after its kRenSection; 0 = none)
		                      // Portalcraft: then water, lava and (since the mcworld suite) not-portalable
		                      // bitsets, 512 bytes each; hosts read what is there
		kRenRagdoll = 9,      // RenAvatar + RenBatch[] + RenVertex[]: the player's body standing still,
		                      // relative to the feet and facing +Z, split into its parts (RenBatch
		                      // flags bits 8-11: RagdollPart). Sent about once a second while alive;
		                      // Skyrim hangs the parts on its ragdoll when the player dies.
	};

	struct RenDug
	{
		std::int32_t  sx, sy, sz;  // section coords, as in RenSection
		std::uint32_t count;       // dug blocks (0: none, and no bitset follows)
		std::uint32_t worldId;     // the host map's dig world (HostPlayer::digWorld) the bits belong to
		std::uint32_t pad;
	};
	static_assert(sizeof(RenDug) == 24);

	// bridge #43: a hole mined in one mover. The cell is a 1-block cube of the mover's own frame: its corner is anchor + (cx, cy, cz) where the anchor is
	// the one the host sent for the mover's shape (ColMoverLattice). The set of dug cells belongs to that mover alone and moves with it.
	struct RenMoverDig
	{
		std::uint32_t moverId;
		std::int32_t  cx, cy, cz;
		std::uint32_t worldId;  // HostPlayer::digWorld when the player mined it (a stale message from another map is dropped)
		std::uint32_t flags;    // bit 0: fill the cell back in (tests); 0: dig it
	};
	static_assert(sizeof(RenMoverDig) == 24);
	inline constexpr std::uint32_t kMoverDigFill = 1u << 0;
	// (bridge #50, flags bits in the same spare word) bit 1: a STYLE message, not a cell: cx = a material kind (ChamberMaterial CH_*), and RenMoverStyle follows the header.
	// Bits 8..15 of a cell's flags: the material kind the cell was mined as (0 = unknown): its walls are drawn with that block's faces.
	inline constexpr std::uint32_t kMoverDigStyle = 1u << 1;
	inline constexpr std::uint32_t kMoverDigKindShift = 8;
	struct RenMoverStyle
	{
		float rects[12];  // the block's atlas rects {u0, v0, u1, v1}: side, top, bottom
	};
	static_assert(sizeof(RenMoverStyle) == 48);

	struct RenSolids
	{
		std::int32_t  sx, sy, sz;  // section coords, as in RenSection
		std::uint32_t count;       // solid blocks (0: none, and no bitset follows)
	};

	enum RagdollPart : std::uint32_t
	{
		kPartNone = 0,
		kPartHead = 1,
		kPartBody = 2,
		kPartRightArm = 3,
		kPartLeftArm = 4,
		kPartRightLeg = 5,
		kPartLeftLeg = 6,
		kPartCount = 7,
	};

	struct RenLights
	{
		std::int32_t  sx, sy, sz;  // section coords, as in RenSection
		std::uint32_t count;
	};

	enum LightKind : std::uint8_t
	{
		kLightSteady = 0,
		kLightFlame = 1,  // torches, fire, campfires, candles: flicker
		kLightLava = 2,   // lava, magma: a slow glow
	};

	enum BlockHazard : std::uint8_t
	{
		kHazardNone = 0,
		kHazardFire = 1,   // fire, soul fire, campfires: burns what stands in it
		kHazardLava = 2,   // lava: burns hard
		kHazardMagma = 3,  // magma block: hurts what stands on top of it
	};

	struct RenLight
	{
		std::uint8_t  x, y, z;  // block within the section
		std::uint8_t  level;    // Minecraft light emission, 1-15
		std::uint32_t color;    // RGB8 (r low byte); top byte: LightKind in bits 0-3, BlockHazard in 4-7
	};
	static_assert(sizeof(RenLight) == 8);

	struct RenAtlasRegion
	{
		std::uint32_t x, y, width, height;  // pixels in the combined atlas (kRenAtlas)
	};

	struct RenScene
	{
		double        originX, originY, originZ;  // MC block the positions are relative to
		std::uint32_t batchCount;
		std::uint32_t vertexCount;
	};

	struct RenTexture
	{
		std::uint32_t id;  // 1+, referenced by RenBatch::texture
		std::uint32_t width, height;
		std::uint32_t pad;
	};

	// The player as Minecraft's own entity renderer draws it (skin, armour, held items, cape),
	// posed and animated, with positions in blocks relative to the player's feet: Skyrim puts
	// it at its own interpolated feet position, the one its camera follows.
	struct RenAvatar
	{
		std::uint32_t batchCount;
		std::uint32_t vertexCount;
	};

	struct RenBatch
	{
		std::uint32_t texture;  // 0: the block/item atlas, else a RenTexture id
		std::uint32_t first;    // first vertex
		std::uint32_t count;    // vertices (multiple of 3)
		std::uint32_t flags;    // bit0: translucent (blended, after the solid pass; casts no shadow)
	};

	struct RenAtlas
	{
		std::uint32_t width, height;
	};

	struct RenSection
	{
		std::int32_t  sx, sy, sz;   // section coords (16-block cubes)
		std::uint32_t vertexCount;  // multiple of 3
	};

	struct RenVertex
	{
		float         x, y, z;  // MC coords relative to the section origin (sx*16, sy*16, sz*16)
		float         u, v;     // atlas UV
		std::uint32_t color;    // RGBA8 (tint * ambient occlusion; Minecraft's fixed face shading is left out)
		std::uint32_t light;    // low byte: block light 0-15, next byte: sky light 0-15
		std::uint32_t flags;    // bit0: cutout (alpha test), bit1: translucent,
		                        // bits 4-6: face normal as MC Direction ordinal + 1 (0 = none: lit without a normal)
	};
	static_assert(sizeof(RenVertex) == 32);

	struct InputEvent
	{
		std::uint16_t type;
		std::uint16_t code;
		std::int32_t  a;
		std::int32_t  b;
		std::int32_t  c;
	};
	static_assert(sizeof(InputEvent) == 16);

	// ---- collision ring @0x20000 (Skyrim produces, MC consumes) -----------------------------
	// Byte ring. Every message starts 8-byte aligned with {u32 type, u32 payloadBytes}.
	// A kColPad message means "skip to the start of the ring".
	inline constexpr std::uint64_t kColRingHeadOff = 0x00;  // u64 total bytes written
	inline constexpr std::uint64_t kColRingTailOff = 0x40;  // u64 total bytes consumed
	inline constexpr std::uint64_t kColRingDataOff = 0x80;
	inline constexpr std::uint64_t kColRingDataBytes = kCollisionRingBytes - kColRingDataOff;

	enum ColType : std::uint32_t
	{
		kColPad = 0,
		kColClear = 1,   // payload: u32 epoch
		kColRegion = 2,  // payload: ColRegion + ColBlock[count]
		kColTris = 3,    // payload: ColRegion (count = triangles) + ColTri[count]; sent before kColRegion
		// 4: bridge #23's kColPlatform, retired in v18
		kColMoverShape = 5,  // v18: payload: ColMoverShape + ColMoverBox[count]: a mover's collision in its own frame
		kColMoverBind = 6,   // v18: payload: ColMoverBind: mover moverId has shape shapeId (0: no collision, removed)
		kColMoverLattice = 7,  // bridge #43 (v18, a spare code): payload: ColMoverLattice: the dig lattice of a shape (sent with every kColMoverShape, after it)
	};

	// Exact Skyrim collision triangle (MC space) for the player's smooth collider.
	enum ColTriFlags : std::uint32_t
	{
		kTriStairHelper = 1u << 0,  // Skyrim's invisible stair ramp: walkable, never a wall
	};

	struct ColTri
	{
		float         v[9];
		std::uint32_t flags;
	};
	static_assert(sizeof(ColTri) == 40);

	struct ColMsgHeader
	{
		std::uint32_t type;
		std::uint32_t payloadBytes;
	};

	// Replaces all Skyrim collision inside the inclusive block box [min, max].
	struct ColRegion
	{
		std::int32_t  minX, minY, minZ;
		std::int32_t  maxX, maxY, maxZ;
		std::uint32_t epoch;
		std::uint32_t count;
	};
	static_assert(sizeof(ColRegion) == 32);

	// ColBlock::pad flags (bridge #33; an older host leaves them 0)
	// bit 0 (bridge #33; no host sets it since bridge #38): the cell is next to Portal 2's solid void outside the map. Digging goes through the void now
	// (bridge #38: the void counts as deep rock), so a mod from #38 on ignores this bit.
	inline constexpr std::uint32_t kColBlockNoDig = 1u << 0;
	// bit 1 (bridge #38): the cell is at the dig limit, near the edge of what Portal 2 can hold (about +-16384 u on any axis): it is revealed as
	// bedrock and never opened.
	inline constexpr std::uint32_t kColBlockLimit = 1u << 1;
	// (bridge #46) bits 2, 3 and 8..12: how much of the cell is geometry you can SEE. The collision field holds every solid the engine collides with, and that
	// includes invisible brushes (clip, invisible walls, light blockers): a player must not walk through them, but digging must treat them as air (no
	// surface to mine, no rock to reveal blocks in: the "blocks in the middle of the air"). Bit 2 says the host filled the rest in (an older host
	// leaves pad bits 2.. 0 and the whole cell counts as visible); bit 3: some solid sub-voxel of the cell is visible; bits 8..12: how many of the cell's
	// 27 reveal sample points (sub-voxels 1, 4, 6 on each axis) are visible solid; the host reports 27 only for a cell that is solid and visible from face to face
	// and at most 24 otherwise, so the mod's "wholly inside" test (25 of 27) means a whole cell, never a wall's surface layer.
	inline constexpr std::uint32_t kColBlockVisKnown = 1u << 2;
	inline constexpr std::uint32_t kColBlockVisAny = 1u << 3;
	inline constexpr std::uint32_t kColBlockVisSamplesShift = 8;
	inline constexpr std::uint32_t kColBlockVisSamplesMask = 31u << 8;

	// What a chamber surface is made of, as the block family it digs into (HostPlayer::digMaterial low byte).
	enum ChamberMaterial : std::uint8_t
	{
		kChNone = 0,        // not known: white panel
		kChPanelWhite = 1,  // portalable white panel
		kChPanelBlack = 2,  // black (not portalable) tile
		kChMetal = 3,       // metal: grates, plating, pipes
		kChTile = 4,        // dark lab tile
		kChGlass = 5,       // glass and windows
		kChConcrete = 6,    // bare concrete (the inside of a wall)
		kChMaterialCount
	};
	inline constexpr std::uint32_t kChPortalableBit = 1u << 8;

	// One block's worth of Skyrim collision as an 8x8x8 occupancy mask.
	// bits[y] bit (z * 8 + x) is sub-voxel (x, y, z), each 1/8 block, in MC axes.
	struct ColBlock
	{
		std::int32_t  x, y, z;
		std::uint32_t pad;
		std::uint64_t bits[8];
	};
	static_assert(sizeof(ColBlock) == 80);

	// ---- v18 (bridge #26): movers ----------------------------------------------------------------------------------
	// Every host solid that can move or animate (doors, lifts, elevators, arm panels, buttons, cubes...) is a "mover".
	// The static field (kColRegion) never holds them. Each mover's collision is voxelised once in its own frame and sent
	// as a shape (kColMoverShape, boxes in MC blocks, mover-local: MC (x, y, z) = Source local (x, z, -y) / 40); movers
	// sharing a model share a shape. kColMoverBind gives a mover its shape (0: none, e.g. it died or turned non-solid).
	// Every host frame the McMovers block holds the pose of the bound movers near the player; Minecraft collides with
	// each shape at its pose, carries a player standing on one by its pose change, and pushes one it moves into.
	struct ColMoverShape
	{
		std::uint32_t shapeId;  // > 0
		std::uint32_t count;    // boxes
	};
	static_assert(sizeof(ColMoverShape) == 8);

	struct ColMoverBox
	{
		float minX, minY, minZ, maxX, maxY, maxZ;  // MC blocks, mover-local
	};
	static_assert(sizeof(ColMoverBox) == 24);

	struct ColMoverBind
	{
		std::uint32_t moverId;  // > 0, never reused in a host session
		std::uint32_t shapeId;  // 0 = unbind
	};
	static_assert(sizeof(ColMoverBind) == 8);

	// (bridge #43) Digging a mover: every shape has a dig lattice, 1-block cubes in the mover's frame (MC blocks) whose corners are the anchor plus
	// whole blocks. The anchor is the corner of the ORIGINAL shape's bounds, so a thin door lies inside one layer of cells; a shape the host rebuilt
	// after a dig (the original minus the dug cells) keeps its anchor. The mod picks the cell under its pickaxe in this lattice and tells the host
	// (kRenMoverDig); the host answers with the new shape (kColMoverShape + kColMoverLattice + kColMoverBind).
	struct ColMoverLattice
	{
		std::uint32_t shapeId;
		std::uint32_t flags;  // bit 0 (kLatticeNoPick): nothing to mine: invisible geometry (a clip brush the player cannot see); bit 1 (kLatticeNoMine): not minable
		float         ax, ay, az;
		std::uint32_t pad;
	};
	static_assert(sizeof(ColMoverLattice) == 24);
	inline constexpr std::uint32_t kLatticeNoPick = 1u << 0;
	inline constexpr std::uint32_t kLatticeNoMine = 1u << 1;  // (bridge #50) solid and pickable but not minable: a physics prop, an NPC, anything Chell can pick up

	// McMovers @0x11080 (seqlock, Host -> MC), between the input ring and the actor table. World point of a mover-local
	// point l (MC blocks): base + pos + R(q) l. Movers not listed this frame have no collision in Minecraft this tick.
	inline constexpr std::uint64_t kOffMcMovers = 0x11080;
	inline constexpr std::uint32_t kMaxMovers = 120;

	struct MoverPose
	{
		std::uint32_t moverId;
		float         x, y, z;         // MC blocks, relative to McMovers' base
		float         qx, qy, qz, qw;  // rotation, MC axes (unit)
	};
	static_assert(sizeof(MoverPose) == 32);

	struct McMovers
	{
		std::uint32_t seq;
		std::uint32_t count;           // <= kMaxMovers
		std::uint32_t epoch;           // the collision epoch the ids belong to
		std::uint32_t flags;
		std::uint64_t frame;           // host frame counter
		double        baseX, baseY, baseZ;  // MC coords (the player's region)
		std::uint32_t pad[4];
		MoverPose     movers[kMaxMovers];
	};
	static_assert(sizeof(McMovers) == 0x40 + 32 * kMaxMovers);
	static_assert(kOffInputRing + kInputRingDataOff + kInputRingEntries * sizeof(InputEvent) <= kOffMcMovers);
	static_assert(kOffMcMovers + sizeof(McMovers) <= kOffActorTable);
}
