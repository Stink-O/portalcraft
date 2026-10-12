#include "MeshBuilder.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "Modules/MaterialSystem.hpp"

void MeshBuilder::ComputeBufferSizes(PrimitiveType type, int max_primitives, int *max_verts, int *max_indices) {
	switch (type) {
		case PrimitiveType::POINTS:
			*max_verts = *max_indices = max_primitives;
			break;
		case PrimitiveType::LINES:
			*max_verts = *max_indices = max_primitives * 2;
			break;
		case PrimitiveType::LINE_STRIP:
			*max_verts = max_primitives + 1;
			*max_indices = max_primitives * 2;
			break;
		case PrimitiveType::LINE_LOOP:
			*max_verts = max_primitives;
			*max_indices = max_primitives * 2;
			break;
		case PrimitiveType::TRIANGLES:
			*max_verts = *max_indices = max_primitives * 3;
			break;
		case PrimitiveType::TRIANGLE_STRIP:
			*max_verts = *max_indices = max_primitives + 2;
			break;
		case PrimitiveType::QUADS:
			*max_verts = max_primitives * 4;
			*max_indices = max_primitives * 6;
			break;
		case PrimitiveType::POLYGON:
			*max_verts = max_primitives;
			*max_indices = (max_primitives - 2) * 3;
			break;
		default:
			// impossible if you're a good lil programmer
			*(int *)0 = 0;
			break;
	}
}

void MeshBuilder::GenerateStripIndices(int *num_indices) {
	int first_idx = this->desc.first_vertex;
	int lines = this->num_verts - 1;
	for (int i = 0; i < lines; ++i) {
		this->desc.index_data[i*2 + 0] = first_idx + i;
		this->desc.index_data[i*2 + 1] = first_idx + i + 1;
	}
	*num_indices = (this->num_verts - 1) * 2;
}

void MeshBuilder::GenerateLoopIndices(int *num_indices) {
	int first_idx = this->desc.first_vertex;
	int lines = this->num_verts - 1;
	for (int i = 0; i < lines; ++i) {
		this->desc.index_data[i*2 + 0] = first_idx + i;
		this->desc.index_data[i*2 + 1] = first_idx + i + 1;
	}
	this->desc.index_data[lines * 2 + 0] = first_idx + lines;
	this->desc.index_data[lines * 2 + 1] = first_idx;
	*num_indices = this->num_verts * 2;
}

void MeshBuilder::GeneratePolygonIndices(int *num_indices) {
	int first_idx = this->desc.first_vertex;
	int tris = this->num_verts - 2;
	for (int i = 0; i < tris; ++i) {
		this->desc.index_data[i*3 + 0] = first_idx;
		this->desc.index_data[i*3 + 1] = first_idx + i + 1;
		this->desc.index_data[i*3 + 2] = first_idx + i + 2;
	}
	*num_indices = (this->num_verts - 2) * 3;
}

void MeshBuilder::GenerateQuadIndices(int *num_indices) {
	int first_idx = this->desc.first_vertex;
	int quads = this->num_verts / 4;
	for (int i = 0; i < quads; ++i) {
		int base_idx = first_idx + i*4;
		this->desc.index_data[i*6 + 0] = base_idx;
		this->desc.index_data[i*6 + 1] = base_idx + 1;
		this->desc.index_data[i*6 + 2] = base_idx + 2;
		this->desc.index_data[i*6 + 3] = base_idx;
		this->desc.index_data[i*6 + 4] = base_idx + 2;
		this->desc.index_data[i*6 + 5] = base_idx + 3;
	}
	*num_indices = this->num_verts / 4 * 6;
}

void MeshBuilder::GenerateDirectIndices(int *num_indices) {
	int first_idx = this->desc.first_vertex;
	for (int i = 0; i < this->num_verts; ++i) {
		this->desc.index_data[i] = first_idx + i;
	}
	*num_indices = this->num_verts;
}

// (bridge #52) Safety net for every dynamic-mesh draw of the plugin: a per-frame vertex budget and a circuit breaker on failed locks.
// The engine's dynamic buffers hand back no memory when they are exhausted (the crashes of 2026-10-05: MeshBuilder::Color, the HUD's
// own dynamic meshes after ours had used them up); past the budget, or after one failed lock in the frame, builders are inert (no lock,
// no draw) until the next frame.
static int g_mbFrameVerts = 0, g_mbFrameDenied = 0, g_mbFrameFails = 0;
static long long g_mbDeniedTotal = 0, g_mbFailTotal = 0;
static const int kMbFrameVertBudget = 450000;
void MeshBuilder::NewFrame() {
	g_mbFrameVerts = 0;
	g_mbFrameFails = 0;
	g_mbFrameDenied = 0;
}
void MeshBuilder::Stats(long long &denied, long long &fails) {
	denied = g_mbDeniedTotal;
	fails = g_mbFailTotal;
}

MeshBuilder::MeshBuilder(IMaterial *material, PrimitiveType type, int max_primitives) {
	this->render_ctx = materialSystem->GetRenderContext();
	this->render_ctx->vtable->BeginRender(this->render_ctx);

	this->mesh = nullptr;
	this->type = type;
	this->num_verts = 0;
	std::memset(&this->desc, 0, sizeof this->desc);
	std::memset(&this->cur_vertex_data, 0, sizeof this->cur_vertex_data);

	int max_verts, max_indices;
	this->ComputeBufferSizes(type, max_primitives, &max_verts, &max_indices);
	this->max_verts_ = max_verts;
	if (max_primitives <= 0 || g_mbFrameFails > 0 || g_mbFrameVerts + max_verts > kMbFrameVertBudget) {
		++g_mbFrameDenied;
		++g_mbDeniedTotal;
		return;  // inert: desc is zero, every setter skips, Draw only releases the context
	}
	g_mbFrameVerts += max_verts;

	this->mesh = this->render_ctx->vtable->GetDynamicMesh(this->render_ctx, true, nullptr, nullptr, material);
	if (!this->mesh) return;

	if (type == PrimitiveType::QUADS || type == PrimitiveType::POLYGON) {
		this->mesh->SetPrimitiveType(PrimitiveType::TRIANGLES);
	} else if (type == PrimitiveType::LINE_STRIP || type == PrimitiveType::LINE_LOOP) {
		this->mesh->SetPrimitiveType(PrimitiveType::LINES);
	} else {
		this->mesh->SetPrimitiveType(type);
	}

	this->mesh->LockMesh(max_verts, max_indices, this->desc);
	if (!this->desc.vertex_data.position || !this->desc.index_data) {
		// the lock gave no memory: nothing may be written, and nothing else is built this frame
		++g_mbFrameFails;
		if (++g_mbFailTotal <= 5) {
			long kb = 0;
			if (FILE *f = std::fopen("/proc/self/status", "r")) {
				char line[128];
				while (std::fgets(line, sizeof line, f)) if (!std::strncmp(line, "VmSize:", 7)) kb = std::atol(line + 7);
				std::fclose(f);
			}
			std::fprintf(stderr, "[bridge] MeshBuilder: dynamic mesh lock of %d vertices failed (frame total %d, VmSize %ld MB)\n", max_verts, g_mbFrameVerts, kb / 1024);
		}
		this->desc.vertex_data.position = nullptr;
	}

	this->cur_vertex_data.position = this->desc.vertex_data.position;
	this->cur_vertex_data.color = this->desc.vertex_data.color;
	for (int i = 0; i < 8; ++i) {
		this->cur_vertex_data.tex_coord[i] = this->desc.vertex_data.tex_coord[i];
	}
}

MeshBuilder::~MeshBuilder() {
	if (this->render_ctx) {
		// the blithering fool of a programmer has failed to call End! they must
		// be shunned. nonetheless, let's try to not break the entire game
		this->render_ctx->vtable->OverrideDepthEnable(this->render_ctx, false, false, false);
		this->render_ctx->vtable->EndRender(this->render_ctx);
		this->render_ctx->vtable->Release(this->render_ctx);
		this->render_ctx = nullptr;
	}
}

void MeshBuilder::ForceDepth(bool write, bool test) {
	if (!this->render_ctx) return;
	this->render_ctx->vtable->OverrideDepthEnable(this->render_ctx, true, write, test);
}

void MeshBuilder::Draw() {
	// Portalcraft (lead, 2026-10-03): the engine hands back no vertex memory when a dynamic mesh asks for more than its
	// buffer holds (a full section of Minecraft terrain did, sp_a4_tb_catch in the terrain world): nothing was written
	// (the setters skip), so draw nothing instead of indexing into nothing.
	if (!this->mesh || !this->desc.vertex_data.position) {
		if (this->mesh) this->mesh->UnlockMesh(0, 0, this->desc);
		this->render_ctx->vtable->OverrideDepthEnable(this->render_ctx, false, false, false);
		this->render_ctx->vtable->EndRender(this->render_ctx);
		this->render_ctx->vtable->Release(this->render_ctx);
		this->render_ctx = nullptr;
		return;
	}
	// generate indices
	int num_indices;
	switch (this->type) {
		case PrimitiveType::LINE_STRIP:
			this->GenerateStripIndices(&num_indices);
			break;
		case PrimitiveType::LINE_LOOP:
			this->GenerateLoopIndices(&num_indices);
			break;
		case PrimitiveType::POLYGON:
			this->GeneratePolygonIndices(&num_indices);
			break;
		case PrimitiveType::QUADS:
			this->GenerateQuadIndices(&num_indices);
			break;
		default:
			this->GenerateDirectIndices(&num_indices);
			break;
	}

	// draw mesh
	this->mesh->UnlockMesh(this->num_verts, num_indices, this->desc);
	this->mesh->Draw();

	// cleanup
	this->render_ctx->vtable->OverrideDepthEnable(this->render_ctx, false, false, false);
	this->render_ctx->vtable->EndRender(this->render_ctx);
	this->render_ctx->vtable->Release(this->render_ctx);
	this->render_ctx = nullptr;
}

void MeshBuilder::Position(Vector pos) {
	if (!this->desc.vertex_data.position || this->num_verts >= this->max_verts_) return;  // (lead) the lock gave no memory: see Draw
	this->cur_vertex_data.position[0] = pos.x;
	this->cur_vertex_data.position[1] = pos.y;
	this->cur_vertex_data.position[2] = pos.z;
}

void MeshBuilder::Color(::Color col) {
	if (!this->desc.vertex_data.position || !this->desc.vertex_data.color || this->num_verts >= this->max_verts_) return;  // (bridge #50: a failed lock leaves the colour pointer at its small offset, not null: w50-b crashed at 0x18)
	this->cur_vertex_data.color[0] = col.b;
	this->cur_vertex_data.color[1] = col.g;
	this->cur_vertex_data.color[2] = col.r;
	this->cur_vertex_data.color[3] = col.a;
}

void MeshBuilder::TexCoord(int stage, float s, float t) {
	if (!this->desc.vertex_data.position || !this->desc.vertex_data.tex_coord[stage] || this->num_verts >= this->max_verts_) return;
	this->cur_vertex_data.tex_coord[stage][0] = s;
	this->cur_vertex_data.tex_coord[stage][1] = t;
}

static void PutVec(float *base, int stride, int n, const float *v, int k) {
	if (!base || stride <= 0) return;
	float *p = (float *)((uintptr_t)base + (uintptr_t)stride * n);
	for (int i = 0; i < k; ++i) p[i] = v[i];
}
void MeshBuilder::Normal(Vector n) {
	if (!this->desc.vertex_data.position || this->num_verts >= this->max_verts_) return;
	float v[3] = {n.x, n.y, n.z};
	PutVec(this->desc.vertex_data.normal, this->desc.vertex_size.normal, this->num_verts, v, 3);
}
void MeshBuilder::TangentS(Vector t) {
	if (!this->desc.vertex_data.position || this->num_verts >= this->max_verts_) return;
	float v[3] = {t.x, t.y, t.z};
	PutVec(this->desc.vertex_data.tangent_s, this->desc.vertex_size.tangent_s, this->num_verts, v, 3);
}
void MeshBuilder::TangentT(Vector t) {
	if (!this->desc.vertex_data.position || this->num_verts >= this->max_verts_) return;
	float v[3] = {t.x, t.y, t.z};
	PutVec(this->desc.vertex_data.tangent_t, this->desc.vertex_size.tangent_t, this->num_verts, v, 3);
}
void MeshBuilder::UserData(float x, float y, float z, float w) {
	if (!this->desc.vertex_data.position || this->num_verts >= this->max_verts_) return;
	float v[4] = {x, y, z, w};
	PutVec(this->desc.vertex_data.user_data, this->desc.vertex_size.user_data, this->num_verts, v, 4);
}

void MeshBuilder::AdvanceVertex() {
	if (this->num_verts >= this->max_verts_) return;  // (bridge #52) more vertices than the lock holds: dropped
#define INC_VERT_PTR(type, field) this->cur_vertex_data.field = (type *)((uintptr_t)this->cur_vertex_data.field + this->desc.vertex_size.field)
	INC_VERT_PTR(float, position);
	INC_VERT_PTR(float, normal);
	INC_VERT_PTR(unsigned char, color);
	for (int i = 0; i < 8; ++i) {
		INC_VERT_PTR(float, tex_coord[i]);
	}
#undef INC_VERT_PTR

	++this->num_verts;
}
