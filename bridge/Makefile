# Portalcraft bridge: 32-bit Portal 2 server plugin (Linux).
#   make            -> build/dev/portalcraft_bridge.so   (experimental; test runs load this)
#   make stable     -> copies the dev build to build/portalcraft_bridge.so (the path users
#                      plugin_load by hand; never replace it while the game is running)
#   make win        -> the Windows plugin build/win/portalcraft_bridge.dll (container; needs podman)
#   make clean
CXX      ?= g++
# RB=gl (default): the OpenGL renderer backend (render_backend/gl_*.cpp). RB=d3d9: the Direct3D 9 backend (src/bridge/d3d9/*.cpp, for
# Portal 2's -vulkan = DXVK) instead of gl_*.cpp. The stubs (render_backend/stub_*.cpp) are for builds without a backend for that feature.
RB       ?= gl
ifeq ($(RB),gl)
SRCS     := $(shell find src -name '*.cpp' ! -path 'src/bridge/d3d9/*' ! -path 'src/bridge/render_backend/stub_*')
else
SRCS     := $(shell find src -name '*.cpp' ! -path 'src/bridge/render_backend/gl_*' ! -path 'src/bridge/render_backend/stub_hud.cpp' ! -path 'src/bridge/render_backend/stub_cut.cpp')
endif
# OBJDIR=<dir> keeps an agent's objects apart from another agent building the same tree at the same time (bridge #24)
OBJDIR   ?= build/obj
OBJS     := $(patsubst src/%.cpp,$(OBJDIR)/%.o,$(SRCS))
DEPS     := $(OBJS:.o=.d)
WARN     := -Wall -Wno-parentheses -Wno-unknown-pragmas -Wno-delete-non-virtual-dtor -Wno-overloaded-virtual -Wno-unused-variable -Wno-class-memaccess -Wno-invalid-offsetof
CXXFLAGS := -std=c++17 -m32 -O2 -g -fPIC -fvisibility=hidden -D_GNU_SOURCE -Isrc/sar -Isrc -Iprotocol $(WARN)
LDFLAGS  := -m32 -shared -Wl,--no-undefined -ldl -lpthread -lrt

# DEVSO=<path> builds elsewhere (e.g. an agent's test build while the user plays the dev one)
DEVSO    ?= build/dev/portalcraft_bridge.so

all: $(DEVSO)

$(DEVSO): $(OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $^ $(LDFLAGS) -o $@

stable: build/dev/portalcraft_bridge.so
	@if pgrep -x portal2_linux >/dev/null; then echo "Portal 2 is running; not replacing the stable build" >&2; exit 1; fi
	cp $< build/portalcraft_bridge.so

$(OBJDIR)/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -c $< -o $@

# make win -> the 32-bit Windows plugin (build/win/portalcraft_bridge.dll), cross-built in a container (see tools/build_windows_plugin.sh)
win:
	tools/build_windows_plugin.sh

clean:
	rm -rf build

-include $(DEPS)
.PHONY: all clean stable win
