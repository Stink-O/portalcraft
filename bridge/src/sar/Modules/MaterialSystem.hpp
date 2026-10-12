#pragma once
// Portalcraft bridge: trimmed SourceAutoRecord MaterialSystem module
// (material/texture creation and the render context), without SAR's
// material-leak and render-context-size patches.
#include "Interface.hpp"
#include "Offsets.hpp"
#include "Utils/SDK.hpp"

struct IMatRenderContext;

#define TEXTUREFLAGS_POINTSAMPLE 0x1

class MaterialSystem {
public:
	Interface *materials = nullptr;

	using _RemoveMaterial = void(__rescall *)(void *thisptr, IMaterialInternal *pMaterial);
	_RemoveMaterial RemoveMaterial = nullptr;
	using _KeyValues_SetString = void(__rescall *)(void *thisptr, const char *key, const char *val);
	_KeyValues_SetString KeyValues_SetString = nullptr;

	bool Init();
	void Shutdown();

	IMaterial *CreateMaterial(const char *name, KeyValues *kv);
	void DestroyMaterial(IMaterial *mat);
	IMaterial *FindMaterial(const char *materialName, const char *textureGroupName);
	ITexture *CreateTexture(const char *name, int w, int h, uint8_t *bgra, int extraFlags = 0);
	void DestroyTexture(ITexture *tex);
	IMatRenderContext *GetRenderContext();
};

extern MaterialSystem *materialSystem;
