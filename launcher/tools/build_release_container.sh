#!/bin/bash
# Builds the launcher against an OLD glibc so it runs on most Linux distributions (Debian 12 / Ubuntu 22.04 and newer).
# A binary built directly on a very new distro (for example Fedora 44, glibc 2.43) refuses to start on older ones.
#   launcher/tools/build_release_container.sh      ->  target-launcher-release/release/portalcraft-launcher
# Needs podman (or docker: CONTAINER=docker) and an internet connection the first time.
set -e
cd "$(dirname "$0")/../.."
CT=${CONTAINER:-podman}
$CT run --rm -v "$PWD:/src:Z" -w /src -e CARGO_TARGET_DIR=/src/target-launcher-release \
  -e CARGO_HOME=/src/target-launcher-release/.cargo-home docker.io/library/rust:1-bookworm \
  cargo build --release --locked --manifest-path launcher/Cargo.toml
echo "built: $PWD/target-launcher-release/release/portalcraft-launcher"
objdump -T target-launcher-release/release/portalcraft-launcher | rg -o 'GLIBC_[0-9.]+' | sort -Vu | tail -1 | sed 's/^/highest glibc needed: /'
