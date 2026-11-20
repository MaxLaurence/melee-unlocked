#!/bin/bash
# The game library (melee_game.so) for the Mac host, built by GCC 14 for Linux inside Docker (GCC is
# required: scalar_storage_order; see docs/linux-native-0.9.md). arm64 runs Debian's own GCC natively
# with the address window above 4 GB (-DMU_WINDOW_BASE, port/mac/elf_loader.h); x86_64 cross-compiles.
# No stack protector and no fortify: the Mac loader binds only plain C library functions.
# usage: port/mac/build-game.sh [arm64|x86_64]   -> build-mac-game/<arch>/melee_game.so
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
ARCH="${1:-arm64}"
command -v docker >/dev/null || { echo "Docker is needed to build the game library (GCC 14 for Linux)"; exit 1; }
docker info >/dev/null 2>&1 || { echo "Start Docker Desktop first"; exit 1; }
WORK="$ROOT/build-mac-game"
mkdir -p "$WORK"
FLAGS="-fno-stack-protector -U_FORTIFY_SOURCE"
if [ "$ARCH" = arm64 ]; then
  IMAGE=melee-unlocked-game-arm64
  printf '%s\n' 'FROM --platform=linux/arm64 debian:trixie' \
    'RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends gcc-14 g++-14 binutils cmake ninja-build python3 && rm -rf /var/lib/apt/lists/*' \
    > "$WORK/Dockerfile.arm64"
  docker build -q -t $IMAGE -f "$WORK/Dockerfile.arm64" "$WORK" >/dev/null
  CONFIGURE="-DCMAKE_C_COMPILER=gcc-14 -DCMAKE_CXX_COMPILER=g++-14 -DMU_WINDOW_BASE=0x10000000000"
else
  IMAGE=melee-unlocked-game-x86_64
  printf '%s\n' 'FROM debian:trixie' \
    'RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends gcc-14-x86-64-linux-gnu g++-14-x86-64-linux-gnu libc6-dev-amd64-cross binutils-x86-64-linux-gnu cmake ninja-build python3 && rm -rf /var/lib/apt/lists/*' \
    > "$WORK/Dockerfile.x86_64"
  printf '%s\n' 'set(CMAKE_SYSTEM_NAME Linux)' 'set(CMAKE_SYSTEM_PROCESSOR x86_64)' \
    'set(CMAKE_C_COMPILER x86_64-linux-gnu-gcc-14)' 'set(CMAKE_CXX_COMPILER x86_64-linux-gnu-g++-14)' \
    'set(CMAKE_FIND_ROOT_PATH /usr/x86_64-linux-gnu)' 'set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)' \
    'set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)' 'set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)' > "$WORK/cross-x86_64.cmake"
  docker build -q -t $IMAGE -f "$WORK/Dockerfile.x86_64" "$WORK" >/dev/null
  CONFIGURE="-DCMAKE_TOOLCHAIN_FILE=/src/build-mac-game/cross-x86_64.cmake -DMELEE_CPU_BASELINE=AVX2"
fi
docker run --rm -v "$ROOT":/src -w /src $IMAGE bash -c "
  set -e
  cmake -S sourceport/game -B build-mac-game/$ARCH -G Ninja -DCMAKE_BUILD_TYPE=Release $CONFIGURE -DCMAKE_C_FLAGS='$FLAGS' -DCMAKE_CXX_FLAGS='$FLAGS' >/dev/null
  cmake --build build-mac-game/$ARCH --target melee_game -j \$(nproc) | grep -v '^\[' || true
  test -f build-mac-game/$ARCH/melee_game.so"
echo "Game library: $WORK/$ARCH/melee_game.so"
