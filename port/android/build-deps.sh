#!/bin/bash
# The Android host's libraries, built with the NDK for arm64-v8a into build-android-deps/arm64:
# SDL2 (shared, with its Java sources, as SDL's Android port expects), glslang with SPIRV-Tools (the
# Vulkan renderer compiles the generated HLSL in the program: HLSL needs SPIRV-Tools' legalization),
# FFmpeg (the game's own media formats only, as the Mac build), OpenSSL and curl (Slippi's web
# services and the WebSocket connections; certificates come from Android's own store).
# usage: port/android/build-deps.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SDK="${ANDROID_HOME:-/opt/homebrew/share/android-commandlinetools}"
NDK="${ANDROID_NDK_HOME:-$SDK/ndk/27.1.12297006}"
API=30
OUT="$ROOT/build-android-deps"
PREFIX="$OUT/arm64"
SRC="$OUT/src"
JOBS="$(sysctl -n hw.ncpu 2>/dev/null || nproc)"
[ -d "$NDK" ] || { echo "No Android NDK at $NDK (set ANDROID_NDK_HOME)"; exit 1; }
TC="$NDK/toolchains/llvm/prebuilt/darwin-x86_64"
[ -d "$TC" ] || TC="$(ls -d "$NDK"/toolchains/llvm/prebuilt/* | head -1)"
mkdir -p "$SRC" "$PREFIX"
COMMON=(-G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake"
        -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-$API -DANDROID_STL=c++_static
        -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_FIND_ROOT_PATH="$PREFIX" -DCMAKE_POLICY_VERSION_MINIMUM=3.5)
clone() { [ -d "$SRC/$1" ] || git clone --depth 1 --branch "$3" "$2" "$SRC/$1"; }
at_commit() {   # name url commit
  [ -d "$SRC/$1/.git" ] && return 0
  git init -q "$SRC/$1" && git -C "$SRC/$1" remote add origin "$2" &&
    git -C "$SRC/$1" fetch -q --depth 1 origin "$3" && git -C "$SRC/$1" checkout -q FETCH_HEAD
}
fetch() {   # tarball url -> $SRC/<dir>
  local file="$SRC/$(basename "$2")"
  [ -d "$SRC/$1" ] && return 0
  [ -f "$file" ] || curl -fsSL "$2" -o "$file"
  tar -xf "$file" -C "$SRC"
}

# ---- SDL2 (shared library + its Java glue)
if [ ! -f "$PREFIX/lib/libSDL2.so" ]; then
  clone SDL2 https://github.com/libsdl-org/SDL.git release-2.32.8
  cmake -S "$SRC/SDL2" -B "$OUT/obj/SDL2" "${COMMON[@]}" -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF
  cmake --build "$OUT/obj/SDL2" && cmake --install "$OUT/obj/SDL2"
fi

# ---- glslang with SPIRV-Tools (the commits glslang 16.0.0 names in known_good.json)
if [ ! -f "$PREFIX/lib/libglslang.a" ]; then
  clone glslang https://github.com/KhronosGroup/glslang.git 16.0.0
  at_commit glslang/External/spirv-tools https://github.com/KhronosGroup/SPIRV-Tools.git 7f2d9ee926f98fc77a3ed1e1e0f113b8c9c49458
  at_commit glslang/External/spirv-tools/external/spirv-headers https://github.com/KhronosGroup/SPIRV-Headers.git 01e0577914a75a2569c846778c2f93aa8e6feddd
  cmake -S "$SRC/glslang" -B "$OUT/obj/glslang" "${COMMON[@]}" -DENABLE_OPT=ON -DENABLE_HLSL=ON \
    -DENABLE_GLSLANG_BINARIES=OFF -DGLSLANG_TESTS=OFF -DBUILD_SHARED_LIBS=OFF -DENABLE_SPVREMAPPER=OFF \
    -DSPIRV_SKIP_TESTS=ON -DSPIRV_SKIP_EXECUTABLES=ON -DSKIP_SPIRV_TOOLS_INSTALL=OFF
  cmake --build "$OUT/obj/glslang" && cmake --install "$OUT/obj/glslang"
fi

# ---- FFmpeg: music packs, video backgrounds and pictures (the Mac build's formats)
FFMPEG=ffmpeg-7.1.1
if [ ! -f "$PREFIX/lib/libavformat.a" ]; then
  fetch $FFMPEG "https://ffmpeg.org/releases/$FFMPEG.tar.xz"
  mkdir -p "$OUT/obj/ffmpeg"
  (cd "$OUT/obj/ffmpeg" && "$SRC/$FFMPEG/configure" --prefix="$PREFIX" --arch=aarch64 --target-os=android \
    --enable-cross-compile --cc="$TC/bin/aarch64-linux-android$API-clang" --cxx="$TC/bin/aarch64-linux-android$API-clang++" \
    --nm="$TC/bin/llvm-nm" --ar="$TC/bin/llvm-ar" --ranlib="$TC/bin/llvm-ranlib" --strip="$TC/bin/llvm-strip" \
    --sysroot="$TC/sysroot" --enable-pic \
    --enable-static --disable-shared --disable-programs --disable-doc --disable-network --disable-autodetect \
    --disable-avdevice --disable-avfilter --disable-everything --enable-zlib --enable-protocol=file \
    --enable-demuxer=mov,matroska,ogg,mp3,aac,flac,wav,aiff,caf,avi,asf,image2,image_png_pipe,image_jpeg_pipe,image_webp_pipe,image_bmp_pipe,image_gif_pipe \
    --enable-decoder=mp3,mp3float,aac,aac_fixed,alac,flac,vorbis,opus,wmav1,wmav2,pcm_s16le,pcm_s16be,pcm_s24le,pcm_s24be,pcm_s32le,pcm_s32be,pcm_f32le,pcm_f32be,pcm_f64le,pcm_u8,pcm_s8,pcm_alaw,pcm_mulaw,adpcm_ms,adpcm_ima_wav,h264,hevc,vp8,vp9,mpeg4,mjpeg,png,bmp,webp,gif \
    --enable-parser=h264,hevc,aac,mpegaudio,flac,vorbis,opus,vp8,vp9,mpeg4video,png,mjpeg,webp,bmp,gif \
    >/dev/null && make -j"$JOBS" >/dev/null && make install >/dev/null)
fi

# ---- OpenSSL (static): curl's TLS and the host's hashing and random numbers
OPENSSL=openssl-3.5.4
if [ ! -f "$PREFIX/lib/libcrypto.a" ]; then
  fetch $OPENSSL "https://github.com/openssl/openssl/releases/download/$OPENSSL/$OPENSSL.tar.gz"
  (cd "$SRC/$OPENSSL" && PATH="$TC/bin:$PATH" ANDROID_NDK_ROOT="$NDK" ./Configure android-arm64 -D__ANDROID_API__=$API \
    --prefix="$PREFIX" --libdir=lib no-shared no-tests no-apps no-docs no-engine no-module >/dev/null &&
    PATH="$TC/bin:$PATH" make -j"$JOBS" build_libs >/dev/null && PATH="$TC/bin:$PATH" make install_dev >/dev/null)
fi

# ---- curl (static, OpenSSL): Android keeps its certificate authorities in OpenSSL's hashed folder layout
CURL=curl-8.17.0
if [ ! -f "$PREFIX/lib/libcurl.a" ]; then
  fetch $CURL "https://curl.se/download/$CURL.tar.xz"
  cmake -S "$SRC/$CURL" -B "$OUT/obj/curl" "${COMMON[@]}" -DBUILD_SHARED_LIBS=OFF -DBUILD_STATIC_LIBS=ON -DBUILD_CURL_EXE=OFF \
    -DBUILD_TESTING=OFF -DBUILD_LIBCURL_DOCS=OFF -DBUILD_MISC_DOCS=OFF -DENABLE_CURL_MANUAL=OFF -DCURL_USE_OPENSSL=ON \
    -DOPENSSL_ROOT_DIR="$PREFIX" -DOPENSSL_USE_STATIC_LIBS=ON -DCURL_CA_BUNDLE=none -DCURL_CA_PATH=/system/etc/security/cacerts \
    -DCURL_USE_LIBPSL=OFF -DCURL_USE_LIBSSH2=OFF -DUSE_NGHTTP2=OFF -DUSE_LIBIDN2=OFF -DCURL_BROTLI=OFF -DCURL_ZSTD=OFF \
    -DCURL_DISABLE_LDAP=ON -DCURL_DISABLE_LDAPS=ON -DENABLE_UNIX_SOCKETS=OFF
  cmake --build "$OUT/obj/curl" && cmake --install "$OUT/obj/curl"
fi
echo "Android dependencies in $PREFIX"
