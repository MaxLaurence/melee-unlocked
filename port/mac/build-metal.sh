#!/bin/bash
# Builds the Mac app (Metal renderer) from a fresh checkout and your own Melee NTSC 1.02 disc.
#   port/mac/build-metal.sh /path/to/melee.iso [arm64|x86_64]
# arm64 (the default) is native on Apple silicon; x86_64 runs on Intel Macs (and under Rosetta).
# Needs: Xcode command line tools, CMake, Ninja, Python 3, Docker Desktop (for the GCC-built game library).
# Result: build-mac-<arch>/MeleeUnlocked.app
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
ISO="${1:?usage: port/mac/build-metal.sh /path/to/melee.iso [arm64|x86_64]}"
ARCH="${2:-arm64}"
cd "$ROOT"
echo "[1/6] Game sources"
git submodule update --init sourceport/extern/melee
python3 tools/prepare_native_sources.py
echo "[2/6] Libraries (SDL2, glslang, SPIRV-Cross) for $ARCH"
bash port/mac/build-deps.sh "$ARCH"
echo "[3/6] Game library for $ARCH (Docker, GCC 14)"
bash port/mac/build-game.sh "$ARCH"
echo "[4/6] Translation headers from the disc"
GEN="$ROOT/build-mac-gen"
mkdir -p "$GEN"
[ -f "$GEN/port-generated/hle_decls.h" ] || {
  python3 tools/extract_dol.py "$ISO" "$GEN/main.dol"
  python3 port/recomp/recomp.py --dol "$GEN/main.dol" --gct-base 0x8065CC80 --out "$GEN/port-generated"; }
echo "[5/6] The app"
BUILD="$ROOT/build-mac-$ARCH"
DEPLOY=13.0; [ "$ARCH" = x86_64 ] && DEPLOY=15.0
cmake -S . -B "$BUILD" -G Ninja -DMELEE_BUILD_MAC=ON -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="$ARCH" \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=$DEPLOY -DMELEE_PORT_GEN="$GEN/port-generated" -DMELEE_MAC_DEPS="$ROOT/build-mac-deps/$ARCH" \
  -DMELEE_MAC_GAME_LIBRARY="$ROOT/build-mac-game/$ARCH/melee_game.so" >/dev/null
# The app only: the root project's unit tests are built and run by the Windows and Linux checks.
cmake --build "$BUILD" --target mac_app
echo "[6/6] Shader seed (hidden runs of the game; once)"
if [ ! -f "$GEN/shader-seed/gx-metal-v1/recipes.bin" ]; then
  python3 port/mac/make_shader_seed.py --app "$BUILD/MeleeUnlocked.app" --iso "$ISO" --out "$GEN/shader-seed"
  cmake --build "$BUILD" --target mac_app   # packages the seed
fi
echo "Done: $BUILD/MeleeUnlocked.app"
