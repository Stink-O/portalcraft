// Portalcraft bridge: engine.so interfaces (client engine + server traces).
#include "bridge.hpp"

#include "Offsets.hpp"
#include "slots.hpp"
#include "platform/platform.hpp"

#include <cstring>

namespace bridge {

Engine engine;

static void __rescall ComputeLightingStandIn(void *, const Vector &, const Vector *, bool, Vector &c, Vector *) { c = Vector(0.5f, 0.5f, 0.5f); }

bool Engine::Init() {
	this->client = Interface::Create(MODULE_NAME("engine"), "VEngineClient015", false);
	this->traceServer = Interface::Create(MODULE_NAME("engine"), "EngineTraceServer004", false);
	this->traceClient = Interface::Create(MODULE_NAME("engine"), "EngineTraceClient004", false);
	if (!this->client || !this->traceServer) return false;
	this->GetLightForPoint = this->client->Original<_GetLightForPoint>(Offsets::GetLightForPoint);
	// ComputeLighting sits at slot 69 in Portal 2's VEngineClient015: GetLightForPoint
	// (slot 1, SAR's offset) forwards to it with a null normal.
	// (Windows safe-load) a held-back slot gets a harmless stand-in that answers mid-grey, so callers need no checks.
	this->ComputeLighting = slots::Orig<_ComputeLighting>(this->client, slots::kClientComputeLighting);
	if (!this->ComputeLighting) this->ComputeLighting = ComputeLightingStandIn;
	this->GetViewAngles = this->client->Original<_GetViewAngles>(Offsets::GetViewAngles);
	this->SetViewAngles = this->client->Original<_SetViewAngles>(Offsets::SetViewAngles);
	this->ClientCmd = this->client->Original<_ClientCmd>(Offsets::ClientCmd);
	// ClientCmd only runs commands flagged FCVAR_CLIENTCMD_CAN_EXECUTE (screenshot
	// yes, setpos / plugin commands no); ExecuteClientCmd runs anything, now.
	this->ExecuteClientCmd = this->client->Original<_ClientCmd>(Offsets::ExecuteClientCmd);
	// IEngineTrace: GetPointContents is slot 0, TraceRay slot Offsets::TraceRay,
	// PointOutsideWorld TraceRay + 14 (same layout SAR relies on).
	this->GetPointContents = slots::Orig<_GetPointContents>(this->traceServer, slots::kTraceGetPointContents);
	this->GetPointContentsWorld = slots::Orig<_GetPointContentsWorld>(this->traceServer, slots::kTraceGetPointContentsWorld);  // world only
	this->GetScreenSize = this->client->Original<_GetScreenSize>(Offsets::GetScreenSize);
	this->Con_IsVisible = this->client->Original<_BoolNoArgs>(Offsets::Con_IsVisible);
	this->IsPaused = this->client->Original<_BoolNoArgs>(Offsets::IsPaused);
	this->TraceRay = this->traceServer->Original<_TraceRay>(Offsets::TraceRay);
	this->PointOutsideWorld = slots::Orig<_PointOutsideWorld>(this->traceServer, slots::kTracePointOutsideWorld);
	return true;
}

void Engine::Shutdown() {
	Interface::Delete(this->client);
	Interface::Delete(this->traceServer);
	Interface::Delete(this->traceClient);
	this->client = this->traceServer = this->traceClient = nullptr;
}

Vector Engine::LightAt(const Vector &p) {
#ifdef _WIN32
	Vector ret(0, 0, 0);
	this->GetLightForPoint(this->client->ThisPtr(), &ret, p, true);
	return ret;
#else
	return this->GetLightForPoint(this->client->ThisPtr(), p, true);
#endif
}

Vector Engine::LightAt(const Vector &p, const Vector &normal) {
	Vector c(0, 0, 0);
	this->ComputeLighting(this->client->ThisPtr(), p, &normal, false, c, nullptr);
	return c;
}

QAngle Engine::ViewAngles() {
	QAngle a{0, 0, 0};
	this->GetViewAngles(this->client->ThisPtr(), a);
	return a;
}

void Engine::SetAngles(const QAngle &a) {
	QAngle copy = a;
	this->SetViewAngles(this->client->ThisPtr(), copy);
}

void Engine::Cmd(const char *cmd) {
	static const bool logCmd = !plat::Getenv("PCB_LOGCMD").empty();  // PCB_LOGCMD=1: every command we send, timestamped (Windows slot bisect)
	if (logCmd) console->Print("[bridge] Cmd %lld ms: %s\n", (long long)(plat::MonotonicNs() / 1000000), cmd);
	this->ExecuteClientCmd(this->client->ThisPtr(), cmd);
}

static void MakeRay(Ray_t &ray, const Vector &start, const Vector &end, const Vector &halfExtents) {
	Vector delta = end - start;
	ray.m_Delta = VectorAligned(delta.x, delta.y, delta.z);
	ray.m_IsSwept = delta.SquaredLength() != 0;
	ray.m_Extents = VectorAligned(halfExtents.x, halfExtents.y, halfExtents.z);
	ray.m_IsRay = halfExtents.SquaredLength() < 1e-6f;
	ray.m_StartOffset = VectorAligned(0, 0, 0);
	ray.m_Start = VectorAligned(start.x, start.y, start.z);
	ray.m_pWorldAxisTransform = nullptr;
}

bool Engine::TraceClient(const Vector &start, const Vector &end, unsigned mask, CGameTrace &tr) {
	struct WorldOnly : public ITraceFilter {
		virtual bool ShouldHitEntity(void *, int) { return false; }
		virtual TraceType_t GetTraceType() const { return TRACE_WORLD_ONLY; }
	} filter;
	Ray_t ray;
	MakeRay(ray, start, end, Vector(0, 0, 0));
	std::memset(&tr, 0, sizeof tr);
	auto fn = this->traceClient->Original<_TraceRay>(Offsets::TraceRay);
	fn(this->traceClient->ThisPtr(), ray, mask, &filter, &tr);
	return tr.fraction < 1.0f || tr.startsolid;
}

bool Engine::Trace(const Vector &start, const Vector &end, const Vector &halfExtents, unsigned mask, CGameTrace &tr, bool worldOnly) {
	struct WorldFilter : public ITraceFilter {
		bool worldOnly;
		virtual bool ShouldHitEntity(void *, int) { return !worldOnly; }
		virtual TraceType_t GetTraceType() const { return worldOnly ? TRACE_WORLD_ONLY : TRACE_EVERYTHING; }
	} filter;
	filter.worldOnly = worldOnly;

	Ray_t ray;
	Vector delta = end - start;
	ray.m_Delta = VectorAligned(delta.x, delta.y, delta.z);
	ray.m_IsSwept = delta.SquaredLength() != 0;
	ray.m_Extents = VectorAligned(halfExtents.x, halfExtents.y, halfExtents.z);
	ray.m_IsRay = halfExtents.SquaredLength() < 1e-6f;
	ray.m_StartOffset = VectorAligned(0, 0, 0);
	ray.m_Start = VectorAligned(start.x, start.y, start.z);
	ray.m_pWorldAxisTransform = nullptr;
	std::memset(&tr, 0, sizeof tr);
	this->TraceRay(this->traceServer->ThisPtr(), ray, mask, &filter, &tr);
	return tr.fraction < 1.0f || tr.startsolid;
}

}  // namespace bridge
