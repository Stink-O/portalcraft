#pragma once
// Portalcraft bridge: trimmed version of SourceAutoRecord's Utils/SDK.hpp.
// Only the SDK pieces the bridge uses are kept.
#include "SDK/Math.hpp"
#include "SDK/Color.hpp"
#include "SDK/UtlMemory.hpp"
#include "SDK/Convar.hpp"
#include "SDK/CViewSetup.hpp"
#include "SDK/ImageFormat.hpp"
#include "SDK/ITexture.hpp"
#include "SDK/KeyValues.hpp"
#include "SDK/MeshBuilder.hpp"
#include "SDK/ServerPlugin.hpp"
#include "SDK/Trace.hpp"

class IMaterial;
class IMaterialInternal;

// From SAR Utils/SDK/Other.hpp
class CMaterial_QueueFriendly {
public:
	void *vtable;
	IMaterialInternal *m_pRealTimeVersion;
};
