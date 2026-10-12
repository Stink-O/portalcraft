// Portalcraft bridge (bridge #70): Portal 2 must register the Minecraft player's jump and crouch.
//
// Portal 2's own logic that reacts to the player's movement (a logic_playerproxy's OnJump / OnDuck / OnUnDuck: sp_a1_intro1's "press space to say
// apple" and the crouch tutorial) is fired by the game's movement code, from the usercmd's buttons. While Minecraft leads, the usercmd is zeroed and
// mc.cpp relays IN_JUMP (a pulse held for 0.15 s: a command made between two network sends is overwritten by the next frame's, so a one-frame pulse
// was lost most of the time: the user's "jumping doesn't trigger the next section") and IN_DUCK (while sneaking).
//
// This file is the safety net. If, 0.35 s after a relayed jump, the server's movement never saw the press (or the game refused it: the body is not on
// the ground for Portal 2), the map's own OnJump connections (read from the BSP's entity lump, mapindex.cpp) are fired here, delay and times-to-fire
// as the map says. A sneak that Portal 2's body never ducked for gets its OnDuck, and the OnUnDuck when the sneak ends.
#include "bridge.hpp"

#include "Modules/Tier1.hpp"

#include <chrono>
#include <cstring>
#include <string>
#include <vector>

namespace bridge {

bool SvJumpSeen(double &at, bool &took);  // blockcollide.cpp
int PuppetPlayerFlags();                  // puppet.cpp

namespace {
double SteadyNow() {
	using namespace std::chrono;
	return duration<double>(steady_clock::now().time_since_epoch()).count();
}
struct Pending {
	double due;
	std::string target, input, param;
};
std::vector<Pending> g_pending;
std::vector<int> g_fired;  // per connection of MapPlayerProxyConns(): times fired
double g_jumpRelayAt = -1;
bool g_jumpChecked = true;
bool g_sneaking = false, g_duckReplayed = false;
double g_sneakSince = 0;
int g_replays = 0;

void Fire(const char *output) {
	const auto &cs = MapPlayerProxyConns();
	if (g_fired.size() != cs.size()) g_fired.assign(cs.size(), 0);
	const double now = SteadyNow();
	int n = 0;
	for (size_t i = 0; i < cs.size(); ++i) {
		const PlayerProxyConn &c = cs[i];
		if (c.output != output) continue;
		if (c.times >= 0 && g_fired[i] >= (c.times == 0 ? 1 : c.times)) continue;
		++g_fired[i];
		g_pending.push_back({now + c.delay, c.target, c.input, c.param});
		++n;
	}
	if (g_replays < 40)
		++g_replays, console->Print("[bridge] player events: replaying %d %s connection(s) of the map's logic_playerproxy\n", n, output);
}
}  // namespace

void PlayerEventsLevelInit() {
	g_pending.clear(), g_fired.clear();
	g_jumpChecked = true, g_sneaking = g_duckReplayed = false;
}

// mc.cpp CreateMove: a jump was relayed (IN_JUMP held from this command on)
void PlayerEventsJumpRelayed() {
	g_jumpRelayAt = SteadyNow();
	g_jumpChecked = false;
}

// mc.cpp CreateMove: whether Minecraft sneaks (every command)
void PlayerEventsSneak(bool sneaking) {
	if (sneaking == g_sneaking) return;
	g_sneaking = sneaking;
	if (sneaking) g_sneakSince = SteadyNow(), g_duckReplayed = false;
	else if (g_duckReplayed) {
		g_duckReplayed = false;
		Fire("OnUnDuck");
	}
}

// mc.cpp Frame
void PlayerEventsFrame() {
	const double now = SteadyNow();
	if (!g_jumpChecked && now - g_jumpRelayAt > 0.35) {
		g_jumpChecked = true;
		double at = -1;
		bool took = false;
		const bool seen = SvJumpSeen(at, took) && at >= g_jumpRelayAt - 0.05;
		const int fl = PuppetPlayerFlags();
		if (seen && took) {
			static int logs = 0;
			if (logs++ < 6) console->Print("[bridge] player events: Portal 2 took the relayed jump (%.0f ms after it), flags 0x%x\n", (at - g_jumpRelayAt) * 1000.0, fl);
		} else {
			static int logs = 0;
			if (logs++ < 12)
				console->Print("[bridge] player events: Portal 2 did NOT take the relayed jump (%s), player flags 0x%x%s: firing the map's OnJump\n",
				               seen ? "its jump check refused it" : "its movement never saw the button", fl, fl >= 0 && !(fl & 1) ? " (not on the ground for Portal 2)" : "");
			Fire("OnJump");
		}
	}
	if (g_sneaking && !g_duckReplayed && now - g_sneakSince > 0.4) {
		const int fl = PuppetPlayerFlags();
		if (fl >= 0 && !(fl & 2)) {
			g_duckReplayed = true;
			console->Print("[bridge] player events: Portal 2's body never ducked for the sneak (flags 0x%x): firing the map's OnDuck\n", fl);
			Fire("OnDuck");
		} else if (fl >= 0)
			g_duckReplayed = false, g_sneakSince = 1e18;  // Portal 2 ducked itself: its own OnDuck / OnUnDuck
	}
	for (size_t i = 0; i < g_pending.size();) {
		if (g_pending[i].due > now) {
			++i;
			continue;
		}
		const Pending p = g_pending[i];
		g_pending.erase(g_pending.begin() + i);
		std::vector<void *> ents;
		if (p.target == "!player") ents = EntFind("player");
		else if (p.target[0] != '!') ents = EntFind(nullptr, p.target.c_str());
		for (void *e : ents) EntInput(e, p.input.c_str(), p.param.empty() ? nullptr : p.param.c_str());
	}
}

}  // namespace bridge
