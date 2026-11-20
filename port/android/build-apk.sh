#!/bin/bash
# Builds the Android app (Vulkan renderer, arm64) from a fresh checkout and your own Melee NTSC 1.02 disc.
#   port/android/build-apk.sh /path/to/melee.iso [--install]
# Needs: the Android SDK (build-tools, platforms;android-34) and NDK 27, a JDK 17, CMake, Ninja, Python 3
# and Docker Desktop (the GCC-built game library). --install puts it on the device adb sees.
# Result: build-android/MeleeUnlocked.apk
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
ISO="${1:?usage: port/android/build-apk.sh /path/to/melee.iso [--install]}"
INSTALL="${2:-}"
SDK="${ANDROID_HOME:-/opt/homebrew/share/android-commandlinetools}"
NDK="${ANDROID_NDK_HOME:-$SDK/ndk/27.1.12297006}"
[ -d /opt/homebrew/opt/openjdk@17/bin ] && export PATH="/opt/homebrew/opt/openjdk@17/bin:$PATH"
cd "$ROOT"
echo "[1/6] Game sources"
git submodule update --init sourceport/extern/melee
python3 tools/prepare_native_sources.py
echo "[2/6] Libraries (SDL2, glslang, FFmpeg, OpenSSL, curl) for Android arm64"
bash port/android/build-deps.sh
echo "[3/6] Game library (Docker, GCC 14, address window at 256 GB)"
bash port/android/build-game.sh
echo "[4/6] Translation headers from the disc"
GEN="$ROOT/build-mac-gen"
mkdir -p "$GEN"
[ -f "$GEN/port-generated/hle_decls.h" ] || {
  python3 tools/extract_dol.py "$ISO" "$GEN/main.dol"
  python3 port/recomp/recomp.py --dol "$GEN/main.dol" --gct-base 0x8065CC80 --out "$GEN/port-generated"; }
echo "[5/6] libmain.so"
cmake -S . -B build-android -G Ninja -DMELEE_BUILD_ANDROID=ON -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-30 \
  -DANDROID_STL=c++_static -DMELEE_PORT_GEN="$GEN/port-generated" -DMELEE_ANDROID_DEPS="$ROOT/build-android-deps/arm64" >/dev/null
cmake --build build-android
echo "[6/6] The APK"
python3 -B port/android/package_apk.py --main build-android/port/android/libmain.so --game build-android-game/melee_game.so \
  --deps build-android-deps/arm64 --out build-android/MeleeUnlocked.apk
if [ "$INSTALL" = --install ]; then adb install -r build-android/MeleeUnlocked.apk; fi
echo "Done: build-android/MeleeUnlocked.apk"
