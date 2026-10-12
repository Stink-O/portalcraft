#include "Interface.hpp"

#include "Modules/Console.hpp"
#include "Utils/Memory.hpp"
#include "Utils/SDK.hpp"
#include "Offsets.hpp"

#include <cstring>
#include <map>
#include <mutex>

namespace {
struct RootVtable {
	uintptr_t *vtable;
	const Interface *owner;
};
std::mutex g_rootMutex;
std::map<void *, RootVtable> &Roots() {
	static std::map<void *, RootVtable> m;
	return m;
}
}  // namespace

uintptr_t *Interface::OriginalVtable(void *object) {
	std::lock_guard<std::mutex> lk(g_rootMutex);
	auto it = Roots().find(object);
	return it == Roots().end() ? nullptr : it->second.vtable;
}

Interface::Interface()
	: baseclass(nullptr)
	, vtable(nullptr)
	, vtableSize(0)
	, isHooked(false)
	, copy(nullptr) {
}
Interface::Interface(void *baseclass, bool copyVtable, bool autoHook)
	: Interface() {
	this->baseclass = reinterpret_cast<uintptr_t **>(baseclass);
	this->vtable = *this->baseclass;

	while (this->vtable[this->vtableSize]) {
		++this->vtableSize;
	}

#ifdef _WIN32
	if (console) console->Print("[bridge] interface %p vtable %p size %d%s\n", baseclass, (void *)this->vtable, this->vtableSize, copyVtable ? " (copy)" : "");
#endif
	if (copyVtable) {
		{  // the first Interface on an object remembers the genuine vtable (later ones would see the earlier one's copy)
			std::lock_guard<std::mutex> lk(g_rootMutex);
			Roots().emplace(baseclass, RootVtable{this->vtable, this});
		}
		this->CopyVtable();
		if (autoHook) {
			this->EnableHooks();
		}
	}
}
Interface::~Interface() {
	this->DisableHooks();
	{
		std::lock_guard<std::mutex> lk(g_rootMutex);
		auto it = Roots().find(this->baseclass);
		if (it != Roots().end() && it->second.owner == this) Roots().erase(it);
	}
	if (this->copy) {
		this->copy.reset();
	}
}
void Interface::CopyVtable() {
	if (!this->copy) {
		// +2: the RTTI word before the table and a zero after it. A later Interface on the same object counts the copy up to that zero;
		// without it the count ran off the end of the heap block (a read past the page on Windows, 2026-10-12).
		this->copy = std::make_unique<uintptr_t[]>(this->vtableSize + 2);
		std::memcpy(this->copy.get(), this->vtable - 1, sizeof(uintptr_t) + this->vtableSize * sizeof(uintptr_t));
	}
}
void Interface::EnableHooks() {
	if (!this->isHooked) {
		*this->baseclass = this->copy.get() + 1;
		this->isHooked = true;
	}
}
void Interface::DisableHooks() {
	if (this->isHooked) {
		*this->baseclass = this->vtable;
		this->isHooked = false;
	}
}
bool Interface::Unhook(int index) {
	if (index >= 0 && index < this->vtableSize) {
		this->copy[index + 1] = this->vtable[index];
		return true;
	}
	return false;
}
Interface *Interface::Create(void *ptr, bool copyVtable, bool autoHook) {
	return (ptr) ? new Interface(ptr, copyVtable, autoHook) : nullptr;
}
Interface *Interface::Create(const char *filename, const char *interfaceSymbol, bool copyVtable, bool autoHook) {
	auto ptr = Interface::GetPtr(filename, interfaceSymbol);
	return Interface::Create(ptr, copyVtable, autoHook);
}
void Interface::Delete(Interface *ptr) {
	if (ptr) {
		delete ptr;
		ptr = nullptr;
	}
}
void *Interface::GetPtr(const char *filename, const char *interfaceSymbol) {
	auto handle = Memory::GetModuleHandleByName(filename);
	if (!handle) {
		console->DevWarning("SAR: Failed to open module %s!\n", filename);
		return nullptr;
	}

	auto CreateInterface = Memory::GetSymbolAddress<CreateInterfaceFn>(handle, "CreateInterface");
	Memory::CloseModuleHandle(handle);

	if (!CreateInterface) {
		console->DevWarning("SAR: Failed to find symbol CreateInterface for %s!\n", filename);
		return nullptr;
	}

	int ret;
	void *fn = CreateInterface(interfaceSymbol, &ret);

	if (ret) {
		uintptr_t CreateInterfaceInternal = 0;
		InterfaceReg *s_pInterfaceRegs = nullptr;
		// make sure offsets are initialized so the GetPtr call for detecting 2011 p2 doesn't explode
		if (Offsets::CreateInterfaceInternal && Offsets::s_pInterfaceRegs) {
			CreateInterfaceInternal = Memory::Read((uintptr_t)CreateInterface + Offsets::CreateInterfaceInternal);
			s_pInterfaceRegs = Memory::DerefDeref<InterfaceReg *>(CreateInterfaceInternal + Offsets::s_pInterfaceRegs);
		}

		if (!CreateInterfaceInternal || !s_pInterfaceRegs) {
			console->DevWarning("SAR: Failed to find interface with symbol %s in %s! (old method failed)\n", interfaceSymbol, filename);
			return nullptr;
		}

		for (auto &current = s_pInterfaceRegs; current; current = current->m_pNext) {
			if (!std::strncmp(current->m_pName, interfaceSymbol, std::strlen(interfaceSymbol))) {
				fn = current->m_CreateFn();
            	// console->DevMsg("SAR: Found interface %s at %p in %s!\n", current->m_pName, fn, filename);
				break;
			}
		}

		if (!fn) {
			// Interface012 -> Interface0
			// Try to get any version of the interface
			if (interfaceSymbol[std::strlen(interfaceSymbol) - 3] >= '0' && interfaceSymbol[std::strlen(interfaceSymbol) - 3] <= '9') {
				char symbol[256];
				std::strncpy(symbol, interfaceSymbol, std::strlen(interfaceSymbol) - 2);
				symbol[std::strlen(interfaceSymbol) - 2] = '\0';
				return Interface::GetPtr(filename, symbol);
			}
			console->DevWarning("SAR: Failed to find interface with symbol %s in %s!\n", interfaceSymbol, filename);
			return nullptr;
		}
	} else {
		// console->DevMsg("SAR: Found interface %s (the easy way) at %p in %s!\n", interfaceSymbol, fn, filename);
	}
	return fn;
}
