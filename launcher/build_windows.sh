#!/bin/bash
# Cross-compiles the Windows launcher on Linux (mingw-w64): launcher/build_windows.sh [--debug]
#   ->  target-launcher-win/x86_64-pc-windows-gnu/release/portalcraft-launcher.exe
# One-time: rustup target add x86_64-pc-windows-gnu; mingw-w64 gcc (Fedora: dnf install mingw64-gcc).
set -e
cd "$(dirname "$0")/.."
export CARGO_TARGET_DIR=target-launcher-win
T=x86_64-pc-windows-gnu
command -v x86_64-w64-mingw32-gcc >/dev/null || { echo "mingw-w64 gcc is missing (Fedora: sudo dnf install mingw64-gcc)"; exit 1; }
rustup target list --installed | rg -q "^$T\$" || rustup target add $T
if [ "${1:-}" = "--debug" ]; then
  cargo build --target $T --manifest-path launcher/Cargo.toml
  echo "built: $PWD/target-launcher-win/$T/debug/portalcraft-launcher.exe"
else
  cargo build --release --target $T --manifest-path launcher/Cargo.toml
  echo "built: $PWD/target-launcher-win/$T/release/portalcraft-launcher.exe"
  objdump -p target-launcher-win/$T/release/portalcraft-launcher.exe | rg -o 'DLL Name: .*' | sort -u | tr '\n' ' '; echo
fi
