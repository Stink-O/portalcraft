#!/bin/bash
# Builds the 32-bit Windows plugin (portalcraft_bridge.dll) with MinGW-w64 in a container, like build_release_plugin.sh does for Linux.
#   bridge/tools/build_windows_plugin.sh      ->  bridge/build/win/portalcraft_bridge.dll
# Needs podman (or docker: CONTAINER=docker) and an internet connection the first time (Ubuntu 22.04, g++-mingw-w64-i686, posix threads).
# PCB_MINGW_IMAGE=<image> uses a prepared image that already has g++-mingw-w64-i686 and make (default: localhost/pcb-mingw32 if it exists).
# Extra arguments are passed to make (e.g. -k for "keep going", OBJDIR=build/objmingw2).
set -e
cd "$(dirname "$0")/.."
CT=${CONTAINER:-podman}
IMG=${PCB_MINGW_IMAGE:-}
if [ -z "$IMG" ]; then
  if $CT image exists localhost/pcb-mingw32 2>/dev/null; then IMG=localhost/pcb-mingw32; else IMG=docker.io/library/ubuntu:22.04; fi
fi
OUT=build/win/portalcraft_bridge.dll
$CT run --rm -v "$PWD:/src:Z" -w /src "$IMG" bash -c '
  set -e
  export DEBIAN_FRONTEND=noninteractive
  if ! command -v i686-w64-mingw32-g++-posix >/dev/null; then
    apt-get update -qq >/dev/null && apt-get install -y -qq g++-mingw-w64-i686 make >/dev/null
  fi
  update-alternatives --set i686-w64-mingw32-g++ /usr/bin/i686-w64-mingw32-g++-posix >/dev/null 2>&1 || true
  make -f Makefile.win -j"$(nproc)" "$@"
  i686-w64-mingw32-strip -s -o build/win/portalcraft_bridge.stripped.dll build/win/portalcraft_bridge.dll   # (no symbol table: a large overlay looks suspicious to antivirus heuristics)' bash "$@"
echo "built: $PWD/$OUT ($(du -h "$OUT" | cut -f1); stripped copy $(du -h build/win/portalcraft_bridge.stripped.dll | cut -f1))"
if command -v x86_64-w64-mingw32-objdump >/dev/null; then OBJDUMP=x86_64-w64-mingw32-objdump; else OBJDUMP=objdump; fi
echo "exports:"; $OBJDUMP -p "$OUT" | sed -n '/Export Address Table/,/^$/p;/\[Ordinal\/Name Pointer\] Table/,/^$/p' | rg -v '^$' || true
echo "imports (DLL names):"; $OBJDUMP -p "$OUT" | rg 'DLL Name' | sort -u
