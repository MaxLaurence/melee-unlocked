#!/bin/bash
# Static SDL2, glslang (HLSL front end) and SPIRV-Cross for the Mac host, built from pinned sources
# into build-mac-deps/<arch>. usage: port/mac/build-deps.sh [arm64|x86_64]   (default arm64)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
ARCH="${1:-arm64}"
OUT="$ROOT/build-mac-deps"
PREFIX="$OUT/$ARCH"
[ -f "$PREFIX/lib/libSDL2.a" ] && [ -f "$PREFIX/lib/libglslang.a" ] && [ -f "$PREFIX/lib/libspirv-cross-msl.a" ] &&
  [ -f "$PREFIX/lib/libavformat.a" ] && { echo "Dependencies for $ARCH already built in $PREFIX"; exit 0; }
mkdir -p "$OUT/src"
COMMON=(-G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="$ARCH" -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0
        -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_POLICY_VERSION_MINIMUM=3.5)
clone() { [ -d "$OUT/src/$1" ] || git clone --depth 1 --branch "$3" "$2" "$OUT/src/$1"; }
clone SDL2 https://github.com/libsdl-org/SDL.git release-2.32.8
clone glslang https://github.com/KhronosGroup/glslang.git 16.0.0
clone SPIRV-Cross https://github.com/KhronosGroup/SPIRV-Cross.git vulkan-sdk-1.4.363.0
cmake -S "$OUT/src/SDL2" -B "$OUT/obj/$ARCH/SDL2" "${COMMON[@]}" -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST=OFF -DSDL_STATIC_PIC=ON
cmake --build "$OUT/obj/$ARCH/SDL2" && cmake --install "$OUT/obj/$ARCH/SDL2"
cmake -S "$OUT/src/glslang" -B "$OUT/obj/$ARCH/glslang" "${COMMON[@]}" -DENABLE_OPT=OFF -DENABLE_HLSL=ON \
  -DENABLE_GLSLANG_BINARIES=OFF -DGLSLANG_TESTS=OFF -DBUILD_SHARED_LIBS=OFF -DENABLE_SPVREMAPPER=OFF
cmake --build "$OUT/obj/$ARCH/glslang" && cmake --install "$OUT/obj/$ARCH/glslang"
cmake -S "$OUT/src/SPIRV-Cross" -B "$OUT/obj/$ARCH/SPIRV-Cross" "${COMMON[@]}" -DSPIRV_CROSS_SHARED=OFF \
  -DSPIRV_CROSS_STATIC=ON -DSPIRV_CROSS_CLI=OFF -DSPIRV_CROSS_ENABLE_TESTS=OFF -DSPIRV_CROSS_ENABLE_HLSL=OFF \
  -DSPIRV_CROSS_ENABLE_CPP=OFF -DSPIRV_CROSS_ENABLE_REFLECT=OFF -DSPIRV_CROSS_ENABLE_C_API=OFF -DSPIRV_CROSS_ENABLE_UTIL=OFF
cmake --build "$OUT/obj/$ARCH/SPIRV-Cross" && cmake --install "$OUT/obj/$ARCH/SPIRV-Cross"
# FFmpeg for music packs, video backgrounds and pictures (port/linux/full/linux_media.cpp): only the
# file formats the game reads (jukebox.cpp: mp3 wav m4a flac ogg; video_background.cpp: mp4; pictures:
# png jpg bmp webp), decoded in software, static, nothing detected from the system.
FFMPEG=ffmpeg-7.1.1
[ -d "$OUT/src/$FFMPEG" ] || { curl -fsSL "https://ffmpeg.org/releases/$FFMPEG.tar.xz" -o "$OUT/src/$FFMPEG.tar.xz"
  tar -xJf "$OUT/src/$FFMPEG.tar.xz" -C "$OUT/src"; }
if [ ! -f "$PREFIX/lib/libavformat.a" ]; then
  FF_ARCH=aarch64; FF_EXTRA=(); [ "$ARCH" = x86_64 ] && { FF_ARCH=x86_64; FF_EXTRA=(--disable-x86asm); }
  [ "$(uname -m)" != "$ARCH" ] && FF_EXTRA+=(--enable-cross-compile)
  mkdir -p "$OUT/obj/$ARCH/ffmpeg"
  (cd "$OUT/obj/$ARCH/ffmpeg" && "$OUT/src/$FFMPEG/configure" --prefix="$PREFIX" --arch=$FF_ARCH --target-os=darwin \
    --cc="clang -arch $ARCH" --extra-cflags="-mmacosx-version-min=13.0" --extra-ldflags="-arch $ARCH -mmacosx-version-min=13.0" \
    --enable-static --disable-shared --disable-programs --disable-doc --disable-network --disable-autodetect \
    --disable-avdevice --disable-avfilter --disable-everything --enable-zlib --enable-protocol=file \
    --enable-demuxer=mov,matroska,ogg,mp3,aac,flac,wav,aiff,caf,avi,asf,image2,image_png_pipe,image_jpeg_pipe,image_webp_pipe,image_bmp_pipe,image_gif_pipe \
    --enable-decoder=mp3,mp3float,aac,aac_fixed,alac,flac,vorbis,opus,wmav1,wmav2,pcm_s16le,pcm_s16be,pcm_s24le,pcm_s24be,pcm_s32le,pcm_s32be,pcm_f32le,pcm_f32be,pcm_f64le,pcm_u8,pcm_s8,pcm_alaw,pcm_mulaw,adpcm_ms,adpcm_ima_wav,h264,hevc,vp8,vp9,mpeg4,mjpeg,png,bmp,webp,gif \
    --enable-parser=h264,hevc,aac,mpegaudio,flac,vorbis,opus,vp8,vp9,mpeg4video,png,mjpeg,webp,bmp,gif \
    ${FF_EXTRA[@]+"${FF_EXTRA[@]}"} >/dev/null && make -j"$(sysctl -n hw.ncpu)" >/dev/null && make install >/dev/null)
fi
echo "Dependencies for $ARCH in $PREFIX"
