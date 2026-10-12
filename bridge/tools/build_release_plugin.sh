#!/bin/bash
# Builds the release plugin so it loads on most Linux distributions, not only the one it was built on.
# A plugin built directly on a brand-new distro (Fedora 44: glibc 2.43) is refused by Portal 2 on older ones
# (Ubuntu 22.04/24.04, Debian 12, Mint), so it is built in an Ubuntu 22.04 container (glibc 2.35) with libstdc++ and
# libgcc linked in statically (no dependency on the Steam runtime's or the host's 32-bit libstdc++).
#   bridge/tools/build_release_plugin.sh      ->  bridge/build/release/portalcraft_bridge.so (stripped)
# Needs podman (or docker: CONTAINER=docker) and an internet connection the first time.
set -e
cd "$(dirname "$0")/.."
CT=${CONTAINER:-podman}
OUT=build/release/portalcraft_bridge.so
rm -rf build/objrelease-ct "$OUT"
$CT run --rm -v "$PWD:/src:Z" -w /src docker.io/library/ubuntu:22.04 bash -c '
  set -e
  export DEBIAN_FRONTEND=noninteractive
  apt-get update -qq >/dev/null && apt-get install -y -qq g++-multilib make >/dev/null
  make -j"$(nproc)" DEVSO=build/release/portalcraft_bridge.so OBJDIR=build/objrelease-ct \
    LDFLAGS="-m32 -shared -Wl,--no-undefined -static-libstdc++ -static-libgcc -ldl -lpthread -lrt" >/dev/null
  strip --strip-debug build/release/portalcraft_bridge.so'
echo "built: $PWD/$OUT ($(du -h "$OUT" | cut -f1))"
objdump -T "$OUT" | grep -o 'GLIBC_[0-9.]*' | sort -Vu | tail -1 | sed 's/^/highest glibc needed: /'
objdump -p "$OUT" | grep NEEDED | sed 's/^ */needs: /'
