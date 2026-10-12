// Portalcraft bridge: Minecraft's sound, heard from where Chell is (protocol v14 HostAudio).
//
// Minecraft keeps its own audio engine (ambience, weather, mobs, water, blocks, items, and Chell's
// stand-in's own footsteps). The plugin drives its listener every frame and its overall level:
//   * Chell's head in the region: the listener is her head mapped into Minecraft, full level;
//   * Chell in a chamber near a portal whose partner is in the region: the listener is her head
//     carried through the pair (what she would hear standing on the far side), the level falling off
//     with her distance to the portal (full within 2 blocks, silent from 10 blocks);
//   * anywhere else, paused, or in a menu: silent.
// The level is smoothed (~80 ms) so crossings and pauses never pop. pc_mc_volume scales it against
// Portal 2 (0 mutes). Portal 2's own sounds are untouched (they do not travel through portals). While
// Chell stands on Minecraft blocks, Portal 2's footsteps are off (Minecraft plays hers).
#include "bridge.hpp"
#include "growth.hpp"
#include "coords.hpp"
#include "link.hpp"

#include "Modules/Tier1.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>

namespace bridge {

using namespace skycraft::proto;
bool ClipHasRegion();
bool ClipRegionContains(const Vector &p);
// (lead) "in Minecraft": the region map's box (pc_pit_flings, the testbed) or the stage / unified world's window onto the terrain
// (every campaign chamber's G-portal world). The ambience fade, the region-portal listener and the footsteps rule only knew the first,
// so the chamber's ambience played on in the campaign's Minecraft world.
static bool InMc(const Vector &p) { return (ClipHasRegion() && ClipRegionContains(p)) || InStageBox(p); }
bool TerrainIsSolid(int bx, int by, int bz);
bool McLevelIsLive();  // mc.cpp
bool McModeActive();
Vector McFeetSrc();

static float g_volume = 0.6f;     // pc_mc_volume
static float g_near = 80.0f, g_far = 400.0f;  // Source units from the portal: full / silent
static float g_gain = 0;          // smoothed
static float g_maxStep = 0;       // largest per-frame change since the last check (smoothness)
static const char *g_phase = "silent";
static int g_log = 0;
static double g_lastT = 0;
bool g_audioTestMute = false;
static bool g_logSounds = false;  // pcb_audio_log_sounds (bridge #23): the mod logs every sound it starts     // tests (on whenever a pcb_script is loaded): Minecraft gets gain 0 while the level is computed as usual

static void AngleVectors3(const QAngle &a, Vector &f, Vector &u) {
	float p = a.x * (float)M_PI / 180, y = a.y * (float)M_PI / 180, r = a.z * (float)M_PI / 180;
	float sp = std::sin(p), cp = std::cos(p), sy = std::sin(y), cy = std::cos(y), sr = std::sin(r), cr = std::cos(r);
	f = Vector(cp * cy, cp * sy, -sp);
	u = Vector(cr * sp * cy + sr * sy, cr * sp * sy - sr * cy, cr * cp);
}
static Vector Cross(const Vector &a, const Vector &b) { return Vector(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
// A point and direction seen through portal i come out of its partner j (half a turn about the up axes).
static Vector Through(int i, int j, const Vector &v, bool point) {
	const Vector &ni = g_portalNormal[i], &ui = g_portalUp[i], &nj = g_portalNormal[j], &uj = g_portalUp[j];
	Vector ri = Cross(ui, ni), rj = Cross(uj, nj);
	Vector d = point ? v - g_portalPos[i] : v;
	Vector o = nj * -Vector::DotProduct(d, ni) + rj * -Vector::DotProduct(d, ri) + uj * Vector::DotProduct(d, ui);
	return point ? g_portalPos[j] + o : o;
}

// Portal 2's footsteps: sv_footsteps' value, set directly (no "cvar changed" notice) while Chell
// stands on Minecraft blocks; restored otherwise.
static ConVar *g_footsteps = nullptr;
static bool g_footOff = false;
static void SetFootsteps(bool on) {
	if (!g_footsteps) {
		g_footsteps = (ConVar *)tier1->FindCommandBase(tier1->g_pCVar->ThisPtr(), "sv_footsteps");
		if (!g_footsteps) return;
	}
	ConVar *v = g_footsteps->m_pParent ? g_footsteps->m_pParent : g_footsteps;
	if (on == !g_footOff) return;
	v->m_nValue = on ? 1 : 0, v->m_fValue = on ? 1.0f : 0.0f;
	g_footOff = !on;
}

// ---- Portal 2's chamber ambience while Chell is in the region (bridge #11, per channel since bridge #12) -------
// The user heard the chamber's ambience in Minecraft: the soundscape (2D; Portal 2 keeps the last one it
// triggered), its room reverb and the map's music. While Chell's eye is in the region they fade out over 0.5 s
// and come back on the way out:
//  - the soundscape: every playing channel whose sound is under ambient/ (all of these maps' soundscape loops and
//    random sounds) has its master volume scaled by the fade (snd_set_master_volume <guid> <0..255>; the channels
//    are read with snd_print_channels, the replies parsed from the console: on entry, every 0.25 s and whenever an
//    ambient/ sound starts, from snd_report_start_sound), and put back on the way out;
//  - music: the mix group "Music" (music/) from its Default_Mix level 0.6 to 0 (snd_setmixer, no cheat flag);
//  - the room DSP preset (dsp_room, set by the soundscape: 104 in the elevator) is held at 0 (none).
// Bridge #11 faded the mix group "Ambient", whose rule is any sound name containing "ambien": that included the
// portals' hum (weapon_ambient/wpn_portal_ambient_lp_01.wav), and the user heard no hum from portals in Minecraft
// (measured: with Ambient at 0 the hum channel's speaker gains go to 0). The Ambient group now stays at 0.6.
// Positional chamber sounds are thousands of units away there; her footsteps, gun, portals (and their hum),
// cubes and the near-portal Minecraft mix are untouched. (Tried first: a plugin-made env_soundscape "Nothing" never
// took over, run-time spawns are not activated; playsoundscape / stopsoundscape and soundscape_fadetime are
// cheats.) pc_mc_ambience 0 turns it off. Every sound command here runs silently (ConRunSilent: the command's own
// callback, its output to the plugin only), so nothing reaches the console or the screen, whatever `developer` is
// (bridge #12 issued them as console text and turned on snd_report_start_sound, whose S_StartSound lines went to the
// console for every sound; a recording with developer >= 1 would show them). New soundscape sounds are found by a
// silent scan every pc_mc_ambience_scan seconds (0.05) instead of those reports.
static bool g_ambEnabled = true;
bool ConRunSilent(const char *line);  // conlog.cpp
static double g_sndMsSum = 0, g_sndMsMax = 0;
static int g_sndN = 0, g_sndLoud = 0;
// a sound command, silently (the console as a fallback, counted: pc_mc_ambience shows it)
static void SndCmd(const char *c) {
	auto t0 = std::chrono::steady_clock::now();
	if (!ConRunSilent(c)) engine.Cmd(c), ++g_sndLoud;
	const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	g_sndMsSum += ms, ++g_sndN;
	if (ms > g_sndMsMax) g_sndMsMax = ms;
}
static double g_ambScanPeriod = 0.05;  // pc_mc_ambience_scan
static float g_amb = 1;            // chamber ambience level (1 chamber, 0 in the region)
static float g_ambSent = 1;        // the level last given to the mixer
static const float kMixAmbient = 0.6f, kMixMusic = 0.6f;  // Default_Mix's levels
static ConVar *g_dspRoom = nullptr;
static int g_dspSaved = -1;        // dsp_room before the region (restored on the way out)
static int g_ambLog = 8;
static float g_mixReadVol = -1;    // the last "VOL:" the engine printed for snd_getmixer (pcb_assert_ambience)
// The channels read back from snd_print_channels (the newest complete listing).
struct SndChan {
	int guid, gain, vol;  // gain: the sum of the five speaker levels the mixer gave it (l c r rl rr)
	Vector pos;
	std::string wave;
};
static std::vector<SndChan> g_chans, g_chansBuild;
PCB_GAUGE("audio.chans", g_chans.size());
static int g_chansSeq = 0;
// soundscape channels under the fade: guid -> natural master volume and the one we set
struct AmbChan {
	int orig, set, seenSeq;
};
static std::unordered_map<int, AmbChan> g_ambChans;
PCB_GAUGE("audio.ambChans", g_ambChans.size());
static bool g_ambStartSeen = false;  // an ambient/ sound started (snd_report_start_sound): scan now
static bool IsSoundscapeWave(const std::string &w) { return w.compare(0, 8, "ambient/") == 0; }
static void ParseChannelLine(const char *m) {
	if (!std::strncmp(m, "Total Channels:", 15)) {
		g_chansBuild.clear();
		return;
	}
	const char *g = std::strstr(m, "guid(");
	if (!g || std::strncmp(m, "index(", 6)) return;
	SndChan c{};
	c.guid = std::atoi(g + 5);
	int sp[5] = {0, 0, 0, 0, 0};
	const char *keys[5] = {" l(", " c(", " r(", " rl(", " rr("};
	for (int k = 0; k < 5; ++k)
		if (const char *q = std::strstr(m, keys[k])) sp[k] = std::atoi(q + std::strlen(keys[k]));
	c.gain = sp[0] + sp[1] + sp[2] + sp[3] + sp[4];
	if (const char *q = std::strstr(m, " vol(")) c.vol = std::atoi(q + 5);
	if (const char *q = std::strstr(m, " pos(")) std::sscanf(q + 5, "%f %f %f", &c.pos.x, &c.pos.y, &c.pos.z);
	if (const char *q = std::strstr(m, "looped(")) {
		q = std::strchr(q, ')');
		if (q) {
			++q;
			while (*q == ' ') ++q;
			while (*q && std::strchr("*#@><^)}$!?&~(", *q)) ++q;  // the sound name's control prefixes
			c.wave = q;
			while (!c.wave.empty() && (c.wave.back() == '\n' || c.wave.back() == '\r' || c.wave.back() == ' ')) c.wave.pop_back();
			std::replace(c.wave.begin(), c.wave.end(), '\\', '/');  // (Windows lists ambient\...: no chamber loop was ever faded there, 2026-10-12)
		}
	}
	g_chansBuild.push_back(c);
	g_chans = g_chansBuild;  // (no end marker: the listing so far is the newest)
	++g_chansSeq;
	if (IsSoundscapeWave(c.wave)) {
		auto it = g_ambChans.find(c.guid);
		if (it == g_ambChans.end()) {
			if (g_ambChans.size() > 512) g_ambChans.clear();  // (bridge #53) channel ids only ever rise: dead ones were never forgotten
			g_ambChans[c.guid] = AmbChan{c.vol, -1, g_chansSeq};
		}
		else {
			if (it->second.set >= 0 && c.vol != it->second.set && c.vol > 0) it->second.orig = c.vol, it->second.set = -1;  // the soundscape set it again
			it->second.seenSeq = g_chansSeq;
		}
	}
}
void AudioLevelInit() {
	g_amb = g_ambSent = 1;
	g_ambChans.clear();
	SndCmd("snd_setmixer Ambient vol 0.600");  // (builds before #12 left it at 0 in the region)
}
void AudioConLine(const char *m) {  // conlog.cpp: engine console lines while the capture is on
	const char *p = std::strstr(m, "VOL: ");
	if (p) g_mixReadVol = (float)std::atof(p + 5);
	ParseChannelLine(m);
	if (const char *q = std::strstr(m, "S_StartSound(\"")) {
		q += 14;
		while (*q && std::strchr("*#@><^)}$!?&~(", *q)) ++q;
		if (!std::strncmp(q, "ambient/", 8) || !std::strncmp(q, "ambient\\", 8)) g_ambStartSeen = true;
	}
}
static void SetMix(float level) {
	char c[96];
	std::snprintf(c, sizeof c, "snd_setmixer Music vol %.3f", kMixMusic * level);
	SndCmd(c);
}
void ConListenAudio(bool on);  // conlog.cpp
// The soundscape channels at the fade level: scan, scale, and put back once the fade is over.
static bool g_ambListening = false;
static double g_ambScanT = 0;
static int g_ambScanSeq = 0;
static void AmbienceChannels(bool active, double now) {
	if (active && !g_ambListening) g_ambListening = true, g_ambScanT = 0;
	if (!g_ambListening) return;
	if (active && (now - g_ambScanT > g_ambScanPeriod || g_ambStartSeen)) {
		SndCmd("snd_print_channels");  // (silent: the listing is parsed by AudioConLine before this returns)
		g_ambScanT = now, g_ambStartSeen = false, g_ambScanSeq = g_chansSeq;
	}
	char c[80];
	for (auto it = g_ambChans.begin(); it != g_ambChans.end();) {
		AmbChan &a = it->second;
		if (g_chansSeq - a.seenSeq > 400 && active) {  // gone (not in the last listings)
			it = g_ambChans.erase(it);
			continue;
		}
		const int want = active ? (int)std::lround(a.orig * g_amb) : a.orig;
		if (want != a.set) {
			std::snprintf(c, sizeof c, "snd_set_master_volume %d %d", it->first, want);
			SndCmd(c);
			a.set = want;
		}
		++it;
	}
	if (!active) {  // restored: stop listening
		g_ambChans.clear();
		g_ambListening = false;
	}
}
static ConVar *DspRoom() {
	if (!g_dspRoom) {
		g_dspRoom = (ConVar *)tier1->FindCommandBase(tier1->g_pCVar->ThisPtr(), "dsp_room");
		if (g_dspRoom && g_dspRoom->m_pParent) g_dspRoom = g_dspRoom->m_pParent;
	}
	return g_dspRoom;
}
static void AmbienceFrame(bool inRegion, float dt) {
	if (!g_ambEnabled) inRegion = false;
	const float target = inRegion ? 0.0f : 1.0f;
	g_amb = target > g_amb ? std::fmin(target, g_amb + 2.0f * dt) : std::fmax(target, g_amb - 2.0f * dt);
	AmbienceChannels(inRegion || g_amb < 1.0f, std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count());
	if (std::fabs(g_amb - g_ambSent) >= 0.05f || (g_amb != g_ambSent && (g_amb == 0 || g_amb == 1))) {
		if ((g_ambSent == 1) != (g_amb == 1) && g_ambLog > 0)
			--g_ambLog, console->Print("[bridge] ambience: chamber ambience %s\n", inRegion ? "fading out (Chell in the region)" : "coming back (Chell left the region)");
		SetMix(g_amb);
		g_ambSent = g_amb;
	}
	// room reverb: none in the region (the soundscape re-sets it; held every frame), the chamber's value back after
	if (ConVar *v = DspRoom()) {
		if (inRegion) {
			if (g_dspSaved < 0) g_dspSaved = v->m_nValue;
			v->m_nValue = 0, v->m_fValue = 0;
		} else if (g_dspSaved >= 0) {
			if (v->m_nValue == 0) v->m_nValue = g_dspSaved, v->m_fValue = (float)g_dspSaved;
			g_dspSaved = -1;
		}
	}
}
void ConLogSet(bool on);  // conlog.cpp
CON_COMMAND(pc_mc_ambience, "pc_mc_ambience [0|1] - fade Portal 2's chamber ambience (soundscape loops, music, room reverb) while Chell is in Minecraft (default 1)\n") {
	if (args.ArgC() > 1) g_ambEnabled = std::atoi(args[1]) != 0;
	console->Print("[bridge] chamber ambience fade %d: level %.2f, dsp_room %d (saved %d); sound commands %d (%d via the console), %.3f ms mean, %.3f ms worst; scan every %.2f s\n", g_ambEnabled, g_amb,
	               DspRoom() ? DspRoom()->m_nValue : -1, g_dspSaved, g_sndN, g_sndLoud, g_sndN ? g_sndMsSum / g_sndN : 0.0, g_sndMsMax, g_ambScanPeriod);
}
CON_COMMAND(pc_mc_ambience_scan, "pc_mc_ambience_scan [s=0.05] - how often the ambience fade looks for new soundscape channels while active\n") {
	if (args.ArgC() > 1) g_ambScanPeriod = std::max(0.0, std::atof(args[1]));
	console->Print("[bridge] ambience scan every %.3f s\n", g_ambScanPeriod);
}
void TestCheck(bool ok, const char *what, const char *detail);
// Reads the mixer back from the engine (snd_getmixer; its reply is captured from the console a few frames later,
// console commands are queued) and dsp_room.
static double g_ambAssertT = 0;  // when the armed check was armed (0: none): developer 1 now, snd_getmixer at +0.15 s, check at +0.35 s
static int g_ambAssertStage = 0;
static bool g_ambAssertRegion = false;
static void AmbienceAssertFrame(double now) {
	if (!g_ambAssertT) return;
	if (g_ambAssertStage == 0 && now - g_ambAssertT > 0.15) {
		g_mixReadVol = -1;
		SndCmd("snd_print_channels");
		engine.Cmd("snd_getmixer Music");  // (test only: its reply needs developer 1, the assert sets it)
		g_ambAssertStage = 1;
	}
	if (g_ambAssertStage != 1 || now - g_ambAssertT < 0.35) return;
	g_ambAssertT = 0, g_ambAssertStage = 0;
	engine.Cmd("developer 0");
	const float vol = g_mixReadVol;
	const int dsp = DspRoom() ? DspRoom()->m_nValue : -1;
	const bool region = g_ambAssertRegion;
	// the soundscape's channels (ambient/...) in the newest listing: silent in Minecraft, back in the chamber
	int scN = 0, scLoud = 0, scGain = 0;
	for (const SndChan &c : g_chans)
		if (IsSoundscapeWave(c.wave)) ++scN, scGain += c.gain, scLoud += c.vol > 0;
	const bool ok = vol >= 0 && (region ? vol <= 0.001f && dsp == 0 && scGain == 0 : std::fabs(vol - kMixMusic) < 0.01f && dsp != 0);
	char buf[260];
	std::snprintf(buf, sizeof buf, "%s: engine's Music mix group vol %.3f (want %s), dsp_room %d (want %s), soundscape channels %d, %d with volume, speaker gain sum %d%s", region ? "in Minecraft" : "in the chamber",
	              vol, region ? "0" : "0.6", dsp, region ? "0" : "the chamber's", scN, scLoud, scGain, region ? " (want 0)" : "");
	TestCheck(ok, "chamber ambience", buf);
}
CON_COMMAND(pcb_assert_ambience, "pcb_assert_ambience <region 1|chamber 0> - (checked 0.35 s later; keep the pose) Portal 2's ambience mix group is silent and the room reverb off in Minecraft (1), or back (0)\n") {
	g_ambAssertRegion = args.ArgC() > 1 && std::atoi(args[1]) != 0;
	ConLogSet(true);
	engine.Cmd("developer 1");
	g_ambAssertT = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(), g_ambAssertStage = 0;
}

// pcb_assert_hum ref | region [min_ratio=0.6]: the portal's hum (weapon_ambient/wpn_portal_ambient_lp_01.wav), the
// loudest such channel's speaker gains (snd_print_channels, read 0.3 s later): "ref" keeps them (Chell ~64 u from a
// chamber portal); "region" checks them >= min_ratio x the reference (Chell ~64 u from a portal in Minecraft), and
// that every soundscape (ambient/) channel is silent.
static double g_humT = 0;
static int g_humStage = 0, g_humRef = -1;
static bool g_humRegion = false;
static float g_humRatio = 0.6f;
static void HumAssertFrame(double now) {
	if (!g_humT) return;
	if (g_humStage == 0 && now - g_humT > 0.05) SndCmd("snd_print_channels"), g_humStage = 1;
	if (g_humStage != 1 || now - g_humT < 0.35) return;
	g_humT = 0, g_humStage = 0;
	int hum = -1, humN = 0, scGain = 0, scN = 0;
	Vector at;
	for (const SndChan &c : g_chans) {
		if (c.wave.find("wpn_portal_ambient_lp") != std::string::npos) {
			++humN;
			if (c.gain > hum) hum = c.gain, at = c.pos;
		}
		if (IsSoundscapeWave(c.wave)) ++scN, scGain += c.gain;
	}
	char buf[260];
	const float d = (at - (PlayerOrigin() + Vector(0, 0, 64))).Length();
	if (!g_humRegion) {
		g_humRef = hum;
		std::snprintf(buf, sizeof buf, "chamber reference: %d hum channels, loudest speaker gain sum %d (portal %.0f u from the eye)", humN, hum, d);
		return TestCheck(hum > 0, "portal hum (chamber reference)", buf);
	}
	std::snprintf(buf, sizeof buf, "in Minecraft: %d hum channels, loudest speaker gain sum %d (portal %.0f u from the eye) vs chamber %d (want >= %.0f%%); soundscape channels %d, gain sum %d (want 0)", humN, hum, d,
	              g_humRef, 100 * g_humRatio, scN, scGain);
	TestCheck(g_humRef > 0 && hum >= g_humRatio * g_humRef && scGain == 0, "portal hum in Minecraft", buf);
}
CON_COMMAND(pcb_assert_hum, "pcb_assert_hum ref | region [min_ratio=0.6] - the portal's hum plays in Minecraft at its chamber loudness (ref: take the chamber reference), the soundscape is silent\n") {
	g_humRegion = args.ArgC() > 1 && !std::strcmp(args[1], "region");
	g_humRatio = args.ArgC() > 2 ? (float)std::atof(args[2]) : 0.6f;
	g_humT = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(), g_humStage = 0;
}

// Main thread, every frame (mc.cpp).
void AudioFrame(bool paused) {
	if (!link::Base()) return;
	double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
	float dt = g_lastT > 0 ? (float)std::fmin(0.1, now - g_lastT) : 0.016f;
	g_lastT = now;
	const Vector feet = PlayerOrigin(), eye = feet + Vector(0, 0, 64);
	const QAngle va = engine.ViewAngles();
	Vector lpos = eye, f, u;
	AngleVectors3(va, f, u);
	float target = 0;
	const char *phase = "silent";
	bool have = false;
	if (!paused) {
		if (InMc(eye)) {
			target = 1, phase = "in the region", have = true;
		} else {
			float best = 1e30f;
			int bi = -1;
			for (int i = 0; i < 2; ++i) {
				int j = 1 - i;
				if (!g_portalSet[i] || !g_portalSet[j] || InMc(g_portalPos[i]) || !InMc(g_portalPos[j])) continue;
				float d = (eye - g_portalPos[i]).Length();
				if (Vector::DotProduct(eye - g_portalPos[i], g_portalNormal[i]) < -8.0f) continue;  // behind the portal's surface
				if (d < best) best = d, bi = i;
			}
			// (bridge #23: the region portal's listener only while it is the louder side; else the chamber's, below)
			const float tp = bi >= 0 ? std::fmax(0.0f, std::fmin(1.0f, (best - g_near) / (g_far - g_near))) : 1.0f;
			if (bi >= 0 && best < g_far && 1 - tp * tp * (3 - 2 * tp) >= 0.5f) {
				float t = tp;
				target = 1 - t * t * (3 - 2 * t);  // smoothstep falloff
				lpos = Through(bi, 1 - bi, eye, true);
				f = Through(bi, 1 - bi, f, false), u = Through(bi, 1 - bi, u, false);
				phase = "near a region portal", have = true;
			}
		}
	}
	// (bridge #23) a Portal 2 chamber: Minecraft's local sounds (the player's own steps, blocks placed and broken, mobs,
	// items, hurt) at full level, heard from the eye mapped into Minecraft (the chamber band is a rigid copy of the
	// chamber); its weather, ambience and music are muted mod-side (kAudioChamber), the band being in the open sky.
	bool chamber = false;
	if (!have && !paused && McLevelIsLive()) {
		target = 1, phase = "in a chamber", have = true, chamber = true;
		if (McModeActive()) lpos = McFeetSrc() + Vector(0, 0, 64);
	}
	target *= g_volume;
	// exponential approach (80 ms), and never more than 0.08 per frame or 6 per second (a hitch or a
	// teleport between two frames still fades)
	float k = 1.0f - std::exp(-dt / 0.08f);
	float lim = std::fmin(0.08f, 6.0f * dt);
	float step = std::fmax(-lim, std::fmin(lim, (target - g_gain) * k));
	g_gain += step;
	if (std::fabs(g_gain - target) < 1e-4f) g_gain = target;
	g_maxStep = std::fmax(g_maxStep, std::fabs(step));
	HostAudio a{};
	if (have || g_gain > 0) {
		double x, y, z;
		SrcToMcD(lpos, x, y, z);
		a.flags = kAudioActive | (chamber ? kAudioChamber : 0u) | (g_logSounds ? kAudioLogSounds : 0u);
		a.x = x, a.y = y, a.z = z;
		float yawSrc = std::atan2(f.y, f.x) * 180.0f / (float)M_PI, pitchSrc = -std::asin(std::fmax(-1.0f, std::fmin(1.0f, f.z))) * 180.0f / (float)M_PI;
		a.yaw = SrcYawToMc(yawSrc), a.pitch = pitchSrc;
	}
	a.gain = g_audioTestMute ? 0.0f : g_gain;
	a.level = g_gain;
	if (!(a.flags & kAudioActive) && g_logSounds) a.flags |= kAudioLogSounds;
	link::WriteHostAudio(a);
	if (phase != g_phase || g_log > 0) {
		if (g_log > 0) --g_log;
		console->Print("[bridge] audio: %s, listener MC (%.1f %.1f %.1f) yaw %.0f pitch %.0f, gain %.2f -> %.2f\n", phase, a.x, a.y, a.z, a.yaw, a.pitch, g_gain, target);
		g_phase = phase;
	}
	AmbienceFrame(InMc(eye), dt);
	AmbienceAssertFrame(now);
	HumAssertFrame(now);
	// Portal 2's footsteps off while Chell stands on Minecraft blocks
	bool onBlocks = false;
	if (InMc(feet)) {
		double x, y, z;
		SrcToMcD(feet - Vector(0, 0, 2), x, y, z);
		onBlocks = TerrainIsSolid((int)std::floor(x), (int)std::floor(y), (int)std::floor(z));
	}
	SetFootsteps(!onBlocks);
}

void AudioShutdown() {
	SetFootsteps(true);
	if (g_ambSent != 1) SetMix(1);
	if (g_dspSaved >= 0 && DspRoom()) DspRoom()->m_nValue = g_dspSaved, DspRoom()->m_fValue = (float)g_dspSaved;
}

CON_COMMAND(pc_mc_volume, "pc_mc_volume [0..1] - Minecraft's level against Portal 2's (0 mutes Minecraft; default 0.6)\n") {
	if (args.ArgC() > 1) g_volume = std::fmax(0.0f, std::fmin(1.0f, (float)std::atof(args[1])));
	console->Print("[bridge] Minecraft volume %.2f\n", g_volume);
}
CON_COMMAND(pcb_audio_test_mute, "pcb_audio_test_mute [0|1] - tests: Minecraft stays silent while its listener and level are driven as usual\n") {
	if (args.ArgC() > 1) g_audioTestMute = std::atoi(args[1]) != 0;
	console->Print("[bridge] audio test mute %d\n", g_audioTestMute);
}
CON_COMMAND(pcb_audio, "pcb_audio [frames] - log the audio listener and gain (for the next N frames)\n") {
	g_log = args.ArgC() > 1 ? std::atoi(args[1]) : 1;
	console->Print("[bridge] audio: %s, gain %.3f, volume %.2f, Portal 2 footsteps %s, largest step %.3f\n", g_phase, g_gain, g_volume, g_footOff ? "off" : "on", g_maxStep);
}
CON_COMMAND(pcb_audio_log_sounds, "pcb_audio_log_sounds [0|1] - (tests) the mod logs each Minecraft sound it starts, its category and the host's level (mc.log)\n") {
	g_logSounds = args.ArgC() > 1 ? std::atoi(args[1]) != 0 : true;
	console->Print("[bridge] audio: sound log %d\n", g_logSounds);
}
void TestCheck(bool ok, const char *what, const char *detail);
CON_COMMAND(pcb_assert_audio, "pcb_assert_audio <min_gain> <max_gain> [max_step=0.1] | reset - the Minecraft gain is in the range and never jumped by more than max_step per frame since the reset\n") {
	if (args.ArgC() > 1 && !std::strcmp(args[1], "reset")) return void(g_maxStep = 0);
	float lo = args.ArgC() > 1 ? (float)std::atof(args[1]) : 0, hi = args.ArgC() > 2 ? (float)std::atof(args[2]) : 1, ms = args.ArgC() > 3 ? (float)std::atof(args[3]) : 0.1f;
	char buf[200];
	std::snprintf(buf, sizeof buf, "%s: gain %.3f (want %.2f..%.2f), largest per-frame step %.3f (max %.2f)", g_phase, g_gain, lo, hi, g_maxStep, ms);
	TestCheck(g_gain >= lo - 1e-3f && g_gain <= hi + 1e-3f && g_maxStep <= ms, "Minecraft audio level", buf);
}

}  // namespace bridge
