package dev.skycraft.link;

/**
 * Mirror of protocol/skycraft_protocol.h. Keep the two in sync.
 */
public final class Proto {
	private Proto() {
	}

	public static final int MAGIC = 0x43594B53;
	public static final int VERSION = 18;
	/** Windows port: the host is a 32-bit DLL in Portal 2 and the link is a named file mapping (WinMapping); everywhere else POSIX shared memory. */
	public static final boolean WINDOWS = System.getProperty("os.name", "").toLowerCase(java.util.Locale.ROOT).contains("win");
	// Portalcraft: POSIX shared-memory name (the file /dev/shm/Portalcraft_v1); on Windows the named file mapping
	// Local\Portalcraft_v1 that the host DLL creates. Override with -Dskycraft.link=/SkyCraft_guest (Windows: Local\SkyCraft_guest)
	// for a second client.
	public static final String MAPPING_NAME = System.getProperty("skycraft.link", WINDOWS ? "Local\\Portalcraft_v1" : "/Portalcraft_v1");
	// Portal 2 host: 40 Source units per block (72 u player = 1.8 blocks).
	public static final double UNITS_PER_BLOCK = 40.0;

	public static final long OFF_HEADER = 0x0;
	public static final long OFF_SKY_STATE = 0x100;
	// Portalcraft (v11): the host camera Minecraft renders its own frame from (HostView).
	public static final long OFF_HOST_VIEW = 0x140;
	public static final long HV_SEQ = 0x00, HV_FLAGS = 0x04, HV_X = 0x08, HV_Y = 0x10, HV_Z = 0x18;
	public static final long HV_YAW = 0x20, HV_PITCH = 0x24, HV_ROLL = 0x28, HV_FOV_Y = 0x2C, HV_PLANE = 0x30, HV_REQUEST_ID = 0x40;
	public static final long HOST_VIEW_BYTES = 0x48;
	public static final int VIEW_ACTIVE = 1, VIEW_CLIP_PLANE = 1 << 1, VIEW_FULL_SCREEN = 1 << 2, VIEW_SCREENSHOT = 1 << 3;
	// Portalcraft (v13): crop windows and a second camera (HostViewExt), right after the HostView.
	public static final long OFF_HOST_VIEW_EXT = 0x188;
	// v14: Minecraft's audio listener, driven by the host (HostAudio)
	public static final long OFF_HOST_AUDIO = 0x80, HOST_AUDIO_BYTES = 0x30;
	public static final int AUDIO_ACTIVE = 1;
	public static final long HA_SEQ = 0x00, HA_FLAGS = 0x04, HA_X = 0x08, HA_Y = 0x10, HA_Z = 0x18, HA_YAW = 0x20, HA_PITCH = 0x24, HA_GAIN = 0x28, HA_LEVEL = 0x2C;
	public static final int AUDIO_CHAMBER = 2, AUDIO_LOG_SOUNDS = 4; // Portalcraft (bridge #23)
	public static final long HOST_VIEW_EXT_BYTES = 0x78;
	public static final int EXT_VALID = 1;
	public static final long HX_SEQ = 0x00, HX_FLAGS = 0x04, HX_CROP = 0x08, HX_PRIMARY_REQUEST_ID = 0x18, HX_PRIMARY_STREAM = 0x1C;
	public static final long HX_SX = 0x20, HX_SY = 0x28, HX_SZ = 0x30, HX_SYAW = 0x38, HX_SPITCH = 0x3C, HX_SROLL = 0x40, HX_SFOV_Y = 0x44;
	public static final long HX_SPLANE = 0x48, HX_SCROP = 0x58, HX_SEC_REQUEST_ID = 0x68, HX_SEC_STREAM = 0x6C, HX_SEC_FLAGS = 0x70, HX_SEC_RENDER_DISTANCE = 0x74;
	// v15: the live portal pair, drawn by Minecraft itself (HostPortals)
	public static final long OFF_HOST_PORTALS = 0xE00, HOST_PORTALS_BYTES = 0xA0, HOST_PORTAL_BYTES = 0x40;
	// v16: light probes (host -> MC) and Minecraft's light at them (MC -> host)
	public static final int MAX_LIGHT_PROBES = 16;
	public static final long OFF_LIGHT_PROBES = 0x16100, OFF_MC_LIGHT = 0x16400;
	public static final long LIGHT_PROBES_BYTES = 0x190, LIGHT_SAMPLE_BYTES = 0x20, MC_LIGHT_BYTES = 0x240;
	public static final long LP_SEQ = 0x00, LP_COUNT = 0x04, LP_REQUEST_ID = 0x08, LP_POS = 0x10;
	public static final long LS_R = 0x00, LS_G = 0x04, LS_B = 0x08, LS_SKY = 0x0C, LS_BLOCK = 0x10, LS_SKY_VISIBLE = 0x14;
	public static final long ML_SEQ = 0x00, ML_REQUEST_ID = 0x04, ML_COUNT = 0x08, ML_FLAGS = 0x0C, ML_SUN_DIR = 0x10, ML_DAYLIGHT = 0x1C;
	public static final long ML_SKY_COLOR = 0x20, ML_DAY_TIME = 0x2C, ML_SKY_LIGHT_COLOR = 0x30, ML_SAMPLES = 0x40;
	public static final int PORTALS_IN_MC = 1;
	public static final long HP_SEQ = 0x00, HP_FLAGS = 0x04, HP_PRIMARY_REQUEST_ID = 0x08, HP_MAX_DEPTH = 0x0C, HP_RENDER_DISTANCE = 0x10, HP_RES_SCALE = 0x14, HP_PORTAL = 0x20;
	public static final long PO_CX = 0x00, PO_CY = 0x08, PO_CZ = 0x10, PO_NX = 0x18, PO_NY = 0x1C, PO_NZ = 0x20, PO_UX = 0x24, PO_UY = 0x28, PO_UZ = 0x2C;
	public static final long PO_HALF_W = 0x30, PO_HALF_H = 0x34, PO_OPEN = 0x38, PO_COLOUR = 0x3C;
	public static final long OFF_MC_STATE = 0x200;
	public static final long OFF_WATER_GRID = 0x400;
	public static final int WATER_GRID_SIZE = 16;
	public static final long WG_SEQ = 0x0, WG_ORIGIN_X = 0x4, WG_ORIGIN_Z = 0x8, WG_WORLD_ID = 0xC, WG_SURFACE = 0x10;
	public static final long OFF_OVERLAY_CTL = 0x300;
	public static final long OFF_OVERLAY_SLOT_HDR = 0x340;
	public static final long OFF_INPUT_RING = 0x1000;
	public static final long OFF_COLLISION_RING = 0x20000;
	public static final long COLLISION_RING_BYTES = 32L << 20;
	public static final long OFF_OVERLAY_PIXELS = OFF_COLLISION_RING + COLLISION_RING_BYTES;
	public static final int MAX_OVERLAY_W = 3840;
	public static final int MAX_OVERLAY_H = 2160;
	public static final long OVERLAY_SLOT_BYTES = (long) MAX_OVERLAY_W * MAX_OVERLAY_H * 4;
	public static final int OVERLAY_SLOTS = 3;
	public static final long OFF_ACTOR_TABLE = 0x12000;
	public static final long OFF_EVENT_RING = 0x17000;
	public static final long OFF_WORLD_ENTITIES = 0x1C000;
	public static final long OFF_RENDER_RING = OFF_OVERLAY_PIXELS + OVERLAY_SLOT_BYTES * OVERLAY_SLOTS;
	public static final long RENDER_RING_BYTES = 64L << 20;
	// v17: the HUD triple buffer after the render ring (the mapping of a v16 host ends at OFF_HUD_PIXELS)
	public static final int MAX_HUD_W = 2048, MAX_HUD_H = 1152;
	public static final long HUD_SLOT_BYTES = (long) MAX_HUD_W * MAX_HUD_H * 4;
	public static final int HUD_SLOTS = 3;
	public static final long OFF_HUD_PIXELS = OFF_RENDER_RING + RENDER_RING_BYTES;
	public static final long MAPPING_BYTES = OFF_HUD_PIXELS + HUD_SLOT_BYTES * HUD_SLOTS;
	public static final long MAPPING_BYTES_V16 = OFF_HUD_PIXELS;
	public static final long OFF_HOST_PLAYER = 0x16800, HOST_PLAYER_BYTES = 0x40;
	public static final int HP_MC_MODE = 1, HP_WANT_HUD = 1 << 1;
	public static final int HP_WANT_AVATAR = 1 << 2, HP_AVATAR_IN_MC = 1 << 3; // bridge #16
	public static final int HP_MC_TRANSIT = 1 << 4; // bridge #28
	public static final int HP_HAND_OVER = 1 << 5; // bridge #31
	public static final int HP_DIG = 1 << 6; // bridge #33: the host digs: kRenDug, ChamberMaterial
	public static final int HP_STAGE = 1 << 7; // bridge #47: the host wants the Minecraft stage (terrain for the portal onto Minecraft) around chunk stageChunkX/Z
	public static final long HPL_SEQ = 0x00, HPL_FLAGS = 0x04, HPL_SCREEN_W = 0x08, HPL_SCREEN_H = 0x0C, HPL_CURSOR_X = 0x10, HPL_CURSOR_Y = 0x14;
	public static final long HPL_HAND_LIGHT = 0x18, HPL_HAND_LIGHT_SEQ = 0x24, HPL_HAND_TONE = 0x28; // bridge #27: the host's light at the hand (3 floats), its seq (0 = unset), tonemap scale
	public static final long HPL_STAGE_CX = 0x34, HPL_STAGE_CZ = 0x38, HPL_STAGE_R = 0x3C; // bridge #47: the stage's centre chunk and radius (chunks), with HP_STAGE
	public static final long HPL_DIG_MATERIAL = 0x2C, HPL_DIG_WORLD = 0x30; // bridge #33: the surface under the crosshair (low byte CH_*, bit 8 portalable), this map's dig world id
	public static final int COL_BLOCK_NO_DIG = 1; // bridge #33: ColBlock.pad bit 0: next to Portal 2's void, never open it
	public static final int COL_BLOCK_VIS_KNOWN = 4; // bridge #46: ColBlock.pad bit 2: the host says which part of the cell is visible geometry (else all of it counts)
	public static final int COL_BLOCK_VIS_ANY = 8; // bridge #46: bit 3: some solid sub-voxel of the cell is visible (not only invisible brushes)
	public static final int COL_BLOCK_VIS_SAMPLES_SHIFT = 8; // bridge #46: bits 8..12: visible solid among the cell's 27 reveal sample points
	public static final int COL_BLOCK_VIS_SAMPLES_MASK = 31 << 8;
	public static final int COL_BLOCK_LIMIT = 2; // bridge #38: ColBlock.pad bit 1: at the dig limit (the edge of what Portal 2 can hold): bedrock, never opened
	public static final int CH_NONE = 0, CH_PANEL_WHITE = 1, CH_PANEL_BLACK = 2, CH_METAL = 3, CH_TILE = 4, CH_GLASS = 5, CH_CONCRETE = 6, CH_PORTALABLE_BIT = 1 << 8;
	public static final long OFF_HUD_CTL = 0x16900, HUD_CTL_BYTES = 0xA0, HUD_SLOT_HDR_BYTES = 0x20;
	public static final long HC_STATE = 0x00, HC_FRAMES_PUBLISHED = 0x08, HC_SLOT = 0x40;
	public static final long HS_WIDTH = 0x00, HS_HEIGHT = 0x04, HS_FLAGS = 0x08, HS_GUI_SCALE = 0x0C, HS_FRAME_ID = 0x10, HS_MC_FLAGS = 0x18, HS_CAMERA_MODE = 0x1C;

	// Input types added in v5
	public static final int IN_HURT = 7;
	public static final int IN_OPEN_MENU = 8;
	public static final int IN_VELOCITY = 9; // Portalcraft: a, b, c = blocks/tick x 10000, applied when the teleport hold releases
	public static final int IN_HOLE = 10; // Portalcraft: a portal hole carved from block collision for players (code: portal | 2 max | 4 clear; MC x 1000)
	public static final int IN_HOST_LIGHT = 11; // Portalcraft (bridge #19): code = Portal 2 light level 0-15 (0x100 clear all); a, b, c = block
	public static final int IN_HAZARD = 12; // Portalcraft (bridge #19): code = kind | 0x10 max corner | 0x20 clear | slot << 8; a, b, c = MC x 1000
	// 14, 15: bridge #23/#25's IN_PLATFORM / IN_PLATFORM_ROT, retired in v18 (movers: OFF_MC_MOVERS)
	public static final int IN_PORTAL = 13; // Portalcraft (bridge #22): code = portal | part << 1 (0 centre x 1000, 1 normal x 1e6, 2 up x 1e6: live) | 0x10 clear
	public static final int HURT_MELEE = 0;
	public static final int HURT_PROJECTILE = 1;
	public static final int HURT_MAGIC = 2;
	public static final int HURT_OTHER = 3;
	public static final int HURT_BLOCKED_IN_SKYRIM = 1;
	public static final int HURT_POWER_ATTACK = 2;

	// Actor table (relative to OFF_ACTOR_TABLE)
	public static final int MAX_ACTORS = 256;
	public static final long AT_SEQ = 0x00;
	public static final long AT_COUNT = 0x04;
	public static final long AT_RECORDS = 0x40;
	public static final long ACTOR_RECORD_BYTES = 64;
	public static final int ACTOR_HOSTILE = 1;
	public static final int ACTOR_DEAD = 1 << 1;
	public static final int ACTOR_ESSENTIAL = 1 << 2;
	public static final int ACTOR_IN_COMBAT = 1 << 3;

	// Event ring (relative to OFF_EVENT_RING)
	public static final int EVENT_RING_ENTRIES = 512;
	public static final long ER_HEAD = 0x00;
	public static final long ER_TAIL = 0x40;
	public static final long ER_DATA = 0x80;
	public static final long EVENT_BYTES = 32;
	public static final int EV_HIT_ACTOR = 1;
	public static final int EV_PLAYER_DIED = 2;
	public static final int EV_EXPLOSION = 3;
	public static final int EV_ARROW_STUCK = 4;
	public static final int EV_SKILL_USE = 5;
	// Skyrim skills (ActorValue) Minecraft reports use of; weapon skills come from EV_HIT_ACTOR.
	public static final int SKILL_BLOCK = 9;
	public static final int SKILL_SMITHING = 10;
	public static final int SKILL_HEAVY_ARMOR = 11;
	public static final int SKILL_LIGHT_ARMOR = 12;
	public static final int HIT_CRITICAL = 1;
	public static final int HIT_PROJECTILE = 1 << 1;
	public static final int HIT_SWEEP = 1 << 2;
	public static final int HIT_FIRE = 1 << 3;
	public static final int WEAPON_UNARMED = 0;
	public static final int WEAPON_BLADE = 1;
	public static final int WEAPON_AXE = 2;
	public static final int WEAPON_BLUNT = 3;
	public static final int WEAPON_PIERCE = 4;
	public static final int WEAPON_ARROW = 5;

	// World entities (relative to OFF_WORLD_ENTITIES)
	public static final int MAX_WORLD_ENTITIES = 160;
	public static final long WE_SEQ = 0x00;
	public static final long WE_COUNT = 0x04;
	public static final long WE_HAS_SELECTION = 0x08;
	public static final long WE_SEL_MIN = 0x0C;
	public static final long WE_SEL_MAX = 0x18;
	public static final long WE_RECORDS = 0x40;
	public static final long WORLD_ENTITY_BYTES = 96;
	public static final int WE_ARROW = 1;
	public static final int WE_ITEM = 2;
	public static final int WE_TRIDENT = 3;
	public static final int WE_BLOCK = 4;
	public static final int WE_CRACK = 5;
	public static final int WE_SHADOW = 6;

	// Render ring (relative to OFF_RENDER_RING)
	public static final long RR_HEAD = 0x00;
	public static final long RR_TAIL = 0x40;
	public static final long RR_DATA = 0x80;
	public static final long RR_DATA_BYTES = RENDER_RING_BYTES - RR_DATA;
	public static final int REN_PAD = 0;
	public static final int REN_ATLAS = 1;
	public static final int REN_SECTION = 2;
	public static final int REN_CLEAR_ALL = 3;
	public static final int REN_TEXTURE = 4;
	public static final int REN_AVATAR = 5;
	public static final int REN_SCENE = 6;
	public static final int REN_ATLAS_REGION = 7;
	public static final int REN_LIGHTS = 8;
	public static final int REN_RAGDOLL = 9;
	public static final int REN_SOLIDS = 10;
	public static final int REN_DUG = 11; // bridge #33: RenDug (24 bytes: sx sy sz count worldId pad) + 512-byte bitset (bit x + 16z + 256y)
	public static final int REN_DUG_BYTES = 24;
	public static final int REN_MOVER_DIG = 12; // bridge #43: RenMoverDig (24 bytes: moverId cx cy cz worldId flags): a cell of a mover's dig lattice was mined
	public static final int REN_MOVER_DIG_BYTES = 24;
	public static final int PART_HEAD = 1, PART_BODY = 2, PART_RIGHT_ARM = 3, PART_LEFT_ARM = 4, PART_RIGHT_LEG = 5, PART_LEFT_LEG = 6;
	public static final int LIGHT_STEADY = 0, LIGHT_FLAME = 1, LIGHT_LAVA = 2;
	public static final int REN_VERTEX_BYTES = 32;

	// Header
	public static final long H_MAGIC = 0x00;
	public static final long H_VERSION = 0x04;
	public static final long H_SKYRIM_PID = 0x08;
	public static final long H_MC_PID = 0x0C;
	public static final long H_SKYRIM_HEARTBEAT = 0x10;
	public static final long H_MC_HEARTBEAT = 0x18;

	// SkyState (relative to OFF_SKY_STATE)
	public static final long SS_SEQ = 0x00;
	public static final long SS_FLAGS = 0x04;
	public static final long SS_WORLD_ID = 0x08;
	public static final long SS_TRANSIT_ACK = 0x0C; // bridge #28 (was SS_COLLISION_EPOCH, never used)
	public static final long SS_POS_X = 0x10;
	public static final long SS_POS_Y = 0x18;
	public static final long SS_POS_Z = 0x20;
	public static final long SS_YAW = 0x28;
	public static final long SS_PITCH = 0x2C;
	public static final long SS_TELEPORT_SEQ = 0x30;
	public static final long SS_VIEWPORT_W = 0x34;
	public static final long SS_VIEWPORT_H = 0x38;
	public static final long SS_GAME_HOUR = 0x3C;

	public static final int SKY_IN_GAME = 1;
	public static final int SKY_MENU_OPEN = 1 << 1;
	public static final int SKY_LOADING = 1 << 2;

	// McState (relative to OFF_MC_STATE)
	public static final long MS_SEQ = 0x00;
	public static final long MS_FLAGS = 0x04;
	public static final long MS_X = 0x08;
	public static final long MS_Y = 0x10;
	public static final long MS_Z = 0x18;
	public static final long MS_YAW = 0x20;
	public static final long MS_PITCH = 0x24;
	public static final long MS_EYE_HEIGHT = 0x28;
	public static final long MS_SENSITIVITY = 0x2C;
	public static final long MS_TELEPORT_ACK = 0x30;
	public static final long MS_GUI_SCALE = 0x34;
	public static final long MS_FRAME_COUNTER = 0x38;
	public static final long MS_FOV = 0x40;
	public static final long MS_BOB_PHASE = 0x44;
	public static final long MS_BOB_AMOUNT = 0x48;
	public static final long MS_EYE_X = 0x50;
	public static final long MS_EYE_Y = 0x58;
	public static final long MS_EYE_Z = 0x60;
	public static final long MS_TICK_QPC = 0x68;
	public static final long MS_PREV_X = 0x70;
	public static final long MS_CUR_X = 0x88;
	public static final long MS_EYE_HEIGHT_O = 0xA0;
	public static final long MS_EYE_HEIGHT_T = 0xA4;
	public static final long MS_WALK_O = 0xA8;
	public static final long MS_WALK = 0xAC;
	public static final long MS_BOB_O = 0xB0;
	public static final long MS_BOB = 0xB4;
	public static final long MS_TICK_MS = 0xB8;
	public static final long MS_CAMERA_MODE = 0xC0;
	public static final long MS_CAMERA_DISTANCE = 0xC4;
	public static final long MS_RIDE_MOVER = 0xC8; // Portalcraft v18 (bridge #26): the mover the player rode this tick
	public static final long MS_RIDE_FLAGS = 0xCC;
	public static final long MS_RIDE_PREV_X = 0xD0;
	public static final long MS_RIDE_CUR_X = 0xE8;
	public static final int RIDE_MOVED = 1, RIDE_PUSHED = 2;
	public static final long MC_STATE_BYTES = 0x100;
	// bridge #28: the Minecraft player's own portal transits (McState spare bytes + McTransit @0x310, written under McState's seqlock)
	public static final long MS_TRANSIT_SEQ = 0x4C, MS_TRANSIT_PORTALS = 0xBC;
	public static final long OFF_MC_TRANSIT = 0x310, MT_Q = 0x00, MT_OFF = 0x10, MT_FRAC = 0x28, MT_SEQ = 0x2C, MC_TRANSIT_BYTES = 0x30;

	public static final int MC_IN_WORLD = 1;
	public static final int MC_SCREEN_OPEN = 1 << 1;
	public static final int MC_ON_GROUND = 1 << 2;
	public static final int MC_SNEAKING = 1 << 3;
	public static final int MC_SPRINTING = 1 << 4;
	public static final int MC_DEAD = 1 << 5;
	public static final int MC_SWIMMING = 1 << 6;
	public static final int MC_FLYING = 1 << 7;

	// Overlay
	public static final long OC_STATE = 0x00;
	public static final long OC_FRAMES_PUBLISHED = 0x08;
	public static final int OVERLAY_DIRTY = 1 << 2;
	public static final long SLOT_HDR_SIZE = 0x40;
	public static final long SH_WIDTH = 0x00;
	public static final long SH_HEIGHT = 0x04;
	public static final long SH_FLAGS = 0x08;
	public static final long SH_FRAME_ID = 0x10;
	public static final long SH_VIEW_REQUEST_ID = 0x18;
	public static final long SH_DEPTH_OFFSET = 0x1C;
	public static final long SH_Z_NEAR = 0x20;
	public static final long SH_Z_FAR = 0x24;
	public static final int SLOT_DEPTH_ALPHA = 1 << 2;
	public static final long SH_DEPTH_W = 0x28;
	public static final long SH_DEPTH_H = 0x2A;
	public static final int SLOT_BOTTOM_UP = 1, SLOT_WORLD_VIEW = 1 << 1;
	public static final long SH_IMG2_OFFSET = 0x2C, SH_IMG2_W = 0x30, SH_IMG2_H = 0x32, SH_IMG2_REQUEST_ID = 0x34, SH_IMG2_FLAGS = 0x38;

	// Input ring (relative to OFF_INPUT_RING)
	public static final int INPUT_RING_ENTRIES = 4096;
	public static final long IR_HEAD = 0x00;
	public static final long IR_TAIL = 0x40;
	public static final long IR_DATA = 0x80;
	public static final int IN_KEY = 1;
	public static final int IN_MOUSE_BUTTON = 2;
	public static final int IN_SCROLL = 3;
	public static final int IN_CURSOR = 4;
	public static final int IN_TEXT = 5;
	public static final int IN_RELEASE_ALL = 6;

	// Collision ring (relative to OFF_COLLISION_RING)
	public static final long CR_HEAD = 0x00;
	public static final long CR_TAIL = 0x40;
	public static final long CR_DATA = 0x80;
	public static final long CR_DATA_BYTES = COLLISION_RING_BYTES - CR_DATA;
	public static final int COL_PAD = 0;
	public static final int COL_CLEAR = 1;
	public static final int COL_REGION = 2;
	public static final int COL_TRIS = 3;
	// 4: bridge #23's COL_PLATFORM, retired in v18
	// v18 (bridge #26): movers (see SkyMovers and the protocol header)
	public static final int COL_MOVER_SHAPE = 5; // ColMoverShape {shapeId, count} + count x {minX..maxZ floats}, MC blocks, mover-local
	public static final int COL_MOVER_BIND = 6; // ColMoverBind {moverId, shapeId (0 = unbind)}
	public static final int COL_MOVER_SHAPE_BYTES = 8;
	public static final int COL_MOVER_BOX_BYTES = 24;
	public static final int COL_MOVER_BIND_BYTES = 8;
	public static final int COL_MOVER_LATTICE = 7; // bridge #43: ColMoverLattice {shapeId, flags, ax, ay, az, pad}: the shape's dig lattice (1-block cubes anchored at ax ay az)
	public static final int COL_MOVER_LATTICE_BYTES = 24;
	public static final int LATTICE_NO_PICK = 1;
	public static final int LATTICE_NO_MINE = 2; // bridge #50: solid and pickable but not minable (physics props, NPCs, anything Chell can pick up)
	public static final int MOVER_DIG_STYLE = 2; // bridge #50: RenMoverDig flags bit 1: a style message (cx = material kind, 12 atlas-rect floats follow the header)
	public static final int MOVER_DIG_KIND_SHIFT = 8; // bridge #50: RenMoverDig flags bits 8..15: the material kind a cell was mined as
	public static final int REN_MOVER_STYLE_BYTES = 48;
	public static final long OFF_MC_MOVERS = 0x11080;
	public static final int MAX_MOVERS = 120;
	public static final long MV_SEQ = 0x00, MV_COUNT = 0x04, MV_EPOCH = 0x08, MV_FLAGS = 0x0C, MV_FRAME = 0x10, MV_BASE_X = 0x18, MV_POSES = 0x40;
	public static final long MOVER_POSE_BYTES = 32;
	public static final long MP_ID = 0x00, MP_X = 0x04, MP_QX = 0x10, MP_QW = 0x1C;
	public static final int COL_TRI_BYTES = 40;
	public static final int TRI_STAIR_HELPER = 1;
	public static final int COL_REGION_HEADER_BYTES = 32;
	public static final int COL_BLOCK_BYTES = 80;
}
