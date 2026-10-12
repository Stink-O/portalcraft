#!/bin/bash
# Builds the launcher: launcher/build.sh [--debug]   ->  target-launcher/release/portalcraft-launcher
set -e
cd "$(dirname "$0")/.."
export CARGO_TARGET_DIR=target-launcher
if [ "${1:-}" = "--debug" ]; then cargo build --manifest-path launcher/Cargo.toml; else cargo build --release --manifest-path launcher/Cargo.toml; fi
