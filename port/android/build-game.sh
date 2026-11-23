#!/bin/bash
# The game library for the Android host: the same GCC 14 Linux AArch64 ELF as the arm64 Mac's
# (port/mac/build-game.sh, the same Docker image), built for an address window at 256 GB. Android's
# arm64 kernels give a process 39 bits of address space (512 GB), so the Mac's 1 TB window does not
# fit, and the low 4 GB belongs to ART's heap. The host loads it itself (port/mac/elf_loader.cpp).
# usage: port/android/build-game.sh   -> build-android-game/melee_game.so
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
command -v docker >/dev/null || { echo "Docker is needed to build the game library (GCC 14 for Linux)"; exit 1; }
docker info >/dev/null 2>&1 || { echo "Start Docker Desktop first"; exit 1; }
WORK="$ROOT/build-android-game"
mkdir -p "$WORK"
IMAGE=melee-unlocked-game-arm64
printf '%s\n' 'FROM --platform=linux/arm64 debian:trixie' \
  'RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends gcc-14 g++-14 binutils cmake ninja-build python3 && rm -rf /var/lib/apt/lists/*' \
  > "$WORK/Dockerfile"
docker build -q -t $IMAGE -f "$WORK/Dockerfile" "$WORK" >/dev/null
FLAGS="-fno-stack-protector -U_FORTIFY_SOURCE"
# Keep in step with kWindowBase in port/mac/elf_loader.h (__ANDROID__).
BASE=0x4000000000
docker run --rm -v "$ROOT":/src -w /src $IMAGE bash -c "
  set -e
  cmake -S sourceport/game -B build-android-game/obj -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc-14 \
    -DCMAKE_CXX_COMPILER=g++-14 -DMU_WINDOW_BASE=$BASE -DCMAKE_C_FLAGS='$FLAGS' -DCMAKE_CXX_FLAGS='$FLAGS' >/dev/null
  # A failed compile must stop here: an older melee_game.so would otherwise pass the check below.
  status=0
  cmake --build build-android-game/obj --target melee_game -j \$(nproc) > build-android-game/build.log 2>&1 || status=\$?
  grep -v '^\[' build-android-game/build.log || true
  [ \$status -eq 0 ] || { echo 'The game library did not build (see above)'; exit \$status; }
  test -f build-android-game/obj/melee_game.so
  cp build-android-game/obj/melee_game.so build-android-game/melee_game.so
  if [ -f build-android-game/obj/melee_game.snapexcl ]; then cp build-android-game/obj/melee_game.snapexcl build-android-game/; fi"
echo "Game library: $WORK/melee_game.so"
