#!/usr/bin/env bash
# Builds the NRO. Run from the devkitPro MSYS2 shell, or from the CI container.
set -e
export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
BUILD_DIR="${BUILD_DIR:-build}"
"$DEVKITPRO/portlibs/switch/bin/aarch64-none-elf-cmake" -S . -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Release "$@"
cmake --build "$BUILD_DIR"
