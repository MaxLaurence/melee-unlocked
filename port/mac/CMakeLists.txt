# The macOS Source Port: the full host (Slippi online, replays, mods, the settings panel) drawn with
# Metal. Native arm64 for Apple silicon (the default), or x86-64 (Intel Macs, and Rosetta).
#
# The game is the same GCC-built Linux ELF as the Linux release (GCC is required for
# scalar_storage_order; docs/linux-native-0.9.md), loaded by the host itself (elf_loader.cpp). Its
# 32-bit disc pointers need MEM1 and the image in one 4 GB window: at zero on x86-64 (a 64 KB
# __PAGEZERO), at 1 TB on arm64, where nothing may be mapped below 4 GB (the game is built with
# -DMU_WINDOW_BASE to match, and its code ships in a signed container, game_container.py).
#
# port/mac/build-metal.sh does everything (dependencies, game library, translation headers, this).
cmake_minimum_required(VERSION 3.25)
enable_language(C OBJCXX)

if(CMAKE_OSX_ARCHITECTURES STREQUAL "arm64")
  set(MELEE_MAC_ARM64 ON)
elseif(CMAKE_OSX_ARCHITECTURES STREQUAL "x86_64")
  set(MELEE_MAC_ARM64 OFF)
else()
  message(FATAL_ERROR "Configure the Mac host for one architecture: -DCMAKE_OSX_ARCHITECTURES=arm64 (or x86_64)")
endif()
set(MELEE_PORT_GEN "" CACHE PATH "The guest translation's generated headers (hle_decls.h, guest_symbols.h)")
set(MELEE_MAC_DEPS "" CACHE PATH "Install prefix of SDL2, glslang and SPIRV-Cross for this architecture (port/mac/build-deps.sh)")
set(MELEE_MAC_GAME_LIBRARY "" CACHE FILEPATH "The Linux melee_game.so for this architecture (port/mac/build-game.sh)")
foreach(needed "${MELEE_PORT_GEN}/hle_decls.h" "${MELEE_PORT_GEN}/guest_symbols.h" "${MELEE_MAC_DEPS}/lib/libSDL2.a"
               "${MELEE_MAC_DEPS}/lib/libglslang.a" "${MELEE_MAC_DEPS}/lib/libspirv-cross-msl.a")
  if(NOT EXISTS "${needed}")
    message(FATAL_ERROR "Missing build input: ${needed}")
  endif()
endforeach()

set(PORT "${PROJECT_SOURCE_DIR}/port")
set(COMPAT "${PORT}/linux/compat")
set(MAC "${CMAKE_CURRENT_SOURCE_DIR}")
file(STRINGS "${PROJECT_SOURCE_DIR}/VERSION" MELEE_PORT_VERSION LIMIT_COUNT 1)
string(REPLACE "." ";" MELEE_VERSION_PARTS "${MELEE_PORT_VERSION}")
list(LENGTH MELEE_VERSION_PARTS MELEE_VERSION_PART_COUNT)
while(MELEE_VERSION_PART_COUNT LESS 4)
  list(APPEND MELEE_VERSION_PARTS "0")
  list(LENGTH MELEE_VERSION_PARTS MELEE_VERSION_PART_COUNT)
endwhile()
list(SUBLIST MELEE_VERSION_PARTS 0 4 MELEE_VERSION_PARTS)
list(JOIN MELEE_VERSION_PARTS "," MELEE_VERSION_COMMAS)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
find_package(Threads REQUIRED)

# x86-64: the game library's instruction set, AVX2 with FMA (the simulation's fused multiply-adds match
# Windows and Slippi Dolphin with it); Rosetta on macOS 15 and later runs it. arm64 always has FMA.
if(MELEE_MAC_ARM64)
  set(MELEE_HOST_ARCH "")
else()
  set(MELEE_HOST_ARCH -mavx2 -mfma)
endif()
set(MELEE_MAC_COMMON_FLAGS -include "${COMPAT}/windows.h" -ffp-contract=off -fno-strict-aliasing
    -Wno-unknown-pragmas -Wno-multichar -Wno-deprecated-declarations -pthread)

# ---- generated sources, as the Windows and Linux builds make them
set(MAC_FOBJ "${CMAKE_CURRENT_BINARY_DIR}/generated/FObjHost.cpp")
add_custom_command(OUTPUT "${MAC_FOBJ}"
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/generate_fobj_host.py" "${MAC_FOBJ}"
  DEPENDS "${PROJECT_SOURCE_DIR}/tools/generate_fobj_host.py" VERBATIM)
add_library(mac_native_animation STATIC "${MAC_FOBJ}")
target_include_directories(mac_native_animation PUBLIC "${PROJECT_SOURCE_DIR}/native")
target_compile_features(mac_native_animation PUBLIC cxx_std_14)
target_compile_options(mac_native_animation PRIVATE ${MELEE_HOST_ARCH} -ffp-contract=off)
set(NATIVE_RECORDING_CODES "${CMAKE_CURRENT_BINARY_DIR}/native_recording_codes.h")
add_custom_command(OUTPUT "${NATIVE_RECORDING_CODES}"
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/native_recording_codes.py" "${NATIVE_RECORDING_CODES}"
  DEPENDS "${PROJECT_SOURCE_DIR}/tools/native_recording_codes.py" "${PORT}/recomp/gecko.py"
          "${PORT}/slippi_sys_general/GameSettings/GALE01r2.ini" "${PORT}/slippi_sys/GameSettings/GALE01r2.ini"
  VERBATIM)

# ---- third-party libraries
file(GLOB MAC_ENET_SOURCES "${PORT}/third_party/enet/*.c")
add_library(mac_enet STATIC ${MAC_ENET_SOURCES})
target_include_directories(mac_enet PUBLIC "${PORT}/third_party/enet/include")
target_compile_definitions(mac_enet PRIVATE HAS_SOCKLEN_T=1 HAS_POLL=1 HAS_FCNTL=1 HAS_MSGHDR_FLAGS=1 HAS_GETADDRINFO=1
  HAS_GETNAMEINFO=1 HAS_INET_PTON=1 HAS_INET_NTOP=1)
target_compile_options(mac_enet PRIVATE -w)
add_library(mac_monocypher STATIC "${PORT}/third_party/monocypher/monocypher.c" "${PORT}/third_party/monocypher/monocypher-ed25519.c")
target_include_directories(mac_monocypher PUBLIC "${PORT}/third_party/monocypher")

add_library(mac_sdl2 INTERFACE)
target_include_directories(mac_sdl2 INTERFACE "${MELEE_MAC_DEPS}/include" "${MELEE_MAC_DEPS}/include/SDL2")
target_link_libraries(mac_sdl2 INTERFACE "${MELEE_MAC_DEPS}/lib/libSDL2.a" iconv
  "-framework Cocoa" "-framework IOKit" "-framework CoreVideo" "-framework CoreAudio" "-framework AudioToolbox"
  "-framework Carbon" "-framework ForceFeedback" "-framework GameController" "-framework CoreHaptics"
  "-framework Metal" "-framework QuartzCore" "-framework AVFoundation" "-framework CoreFoundation"
  "-weak_framework UniformTypeIdentifiers")

add_library(mac_imgui STATIC
  "${PORT}/third_party/imgui/imgui.cpp" "${PORT}/third_party/imgui/imgui_draw.cpp"
  "${PORT}/third_party/imgui/imgui_tables.cpp" "${PORT}/third_party/imgui/imgui_widgets.cpp"
  "${PORT}/third_party/imgui/backends/imgui_impl_sdl2.cpp" "${PORT}/third_party/imgui/backends/imgui_impl_metal.mm"
  "${MAC}/imgui_metal_bridge.mm")
target_include_directories(mac_imgui PUBLIC "${PORT}/third_party/imgui" "${PORT}/third_party/imgui/backends" "${MAC}")
target_compile_features(mac_imgui PUBLIC cxx_std_17)
target_compile_options(mac_imgui PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
target_link_libraries(mac_imgui PUBLIC mac_sdl2)

# HLSL to MSL in the program (shader_msl.cpp): glslang and SPIRV-Cross, built by build-deps.sh.
add_library(mac_shaders STATIC "${MAC}/shader_msl.cpp")
target_include_directories(mac_shaders PUBLIC "${MAC}" PRIVATE "${MELEE_MAC_DEPS}/include")
target_compile_definitions(mac_shaders PRIVATE ENABLE_HLSL)
target_compile_features(mac_shaders PUBLIC cxx_std_17)
set(MELEE_MAC_GLSLANG_LIBS glslang MachineIndependent GenericCodeGen OSDependent SPIRV glslang-default-resource-limits)
foreach(lib ${MELEE_MAC_GLSLANG_LIBS} spirv-cross-msl spirv-cross-glsl spirv-cross-core)
  target_link_libraries(mac_shaders PRIVATE "${MELEE_MAC_DEPS}/lib/lib${lib}.a")
endforeach()

# Metal, AppKit and the ELF loader: Objective-C++ and plain C++ that must not see the Win32
# compatibility headers (they typedef BOOL, which Objective-C defines differently).
# libusb (the vendored 1.0.27, its darwin backend) for the GameCube adapter, as Dolphin reads it on a
# Mac: GCAdapterDriver keeps Apple's HID driver off the adapter so the interface can be claimed.
set(LIBUSB "${PORT}/third_party/libusb")
add_library(mac_libusb STATIC "${LIBUSB}/libusb/core.c" "${LIBUSB}/libusb/descriptor.c" "${LIBUSB}/libusb/hotplug.c"
  "${LIBUSB}/libusb/io.c" "${LIBUSB}/libusb/strerror.c" "${LIBUSB}/libusb/sync.c" "${LIBUSB}/libusb/os/darwin_usb.c"
  "${LIBUSB}/libusb/os/events_posix.c" "${LIBUSB}/libusb/os/threads_posix.c")
target_include_directories(mac_libusb PRIVATE "${LIBUSB}/Xcode" PUBLIC "${LIBUSB}/libusb")
target_compile_options(mac_libusb PRIVATE -w)
target_link_libraries(mac_libusb PUBLIC "-framework IOKit" "-framework CoreFoundation" "-framework Security")

add_library(mac_platform STATIC "${MAC}/metal_device.mm" "${MAC}/mac_window.mm" "${MAC}/iso_picker.mm" "${MAC}/main_thread.mm" "${MAC}/elf_loader.cpp")
target_include_directories(mac_platform PUBLIC "${MAC}" PRIVATE "${PORT}/runtime/abi")
target_compile_features(mac_platform PUBLIC cxx_std_17)
target_compile_options(mac_platform PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
target_link_libraries(mac_platform PUBLIC mac_sdl2)
# The YouGame player-box adapter: manual reference counting, as it was written.
add_library(mac_embed STATIC "${MAC}/embed_mac.mm")
target_include_directories(mac_embed PRIVATE "${PORT}/runtime/hle" "${PORT}/runtime/host" "${MAC}")
target_compile_features(mac_embed PRIVATE cxx_std_17)
target_compile_options(mac_embed PRIVATE -fblocks -fno-objc-arc)
target_link_libraries(mac_embed PUBLIC mac_sdl2)

include("${PORT}/app/disc_rvz.cmake")

# ---- the host runtime: every portable file of runtime/, the Windows renderers and GPU libraries and
# the Vulkan renderer left out, the Linux implementations and the Metal renderer added.
file(GLOB MAC_RUNTIME_SOURCES "${PORT}/runtime/ppc/*.cpp" "${PORT}/runtime/hle/*.cpp" "${PORT}/runtime/host/*.cpp"
  "${PORT}/runtime/gx/*.cpp" "${PORT}/runtime/mu_net/*.cpp")
foreach(removed
    runtime/ppc/leaf_stencils.cpp runtime/ppc/leaf_translator.cpp runtime/ppc/ram_translator.cpp
    runtime/gx/gx_d3d11.cpp runtime/gx/gx_d3d12.cpp runtime/gx/gx_dlss5.cpp runtime/gx/gx_dlss5_scaling.cpp
    runtime/gx/gx_dxr_gpu_scene.cpp runtime/gx/gx_dxr_path_tracer.cpp runtime/gx/gx_streamline.cpp runtime/gx/gx_xess.cpp
    runtime/gx/gx_fsr.cpp runtime/gx/gx_fsr_vk.cpp runtime/gx/rtx_scene.cpp runtime/gx/rtx_tracer.cpp
    runtime/gx/rtx_melee_integrator.cpp runtime/gx/gx_backend.cpp runtime/gx/pc_settings_dx11.cpp runtime/host/audio.cpp
    runtime/gx/gx_dxc_linux.cpp runtime/gx/gx_vulkan.cpp)
  list(REMOVE_ITEM MAC_RUNTIME_SOURCES "${PORT}/${removed}")
endforeach()
add_library(mac_runtime STATIC ${MAC_RUNTIME_SOURCES} "${PORT}/third_party/slippilib/SlippiGame.cpp" "${PORT}/app/launcher_replay_data.cpp"
  "${COMPAT}/compat.cpp" "${COMPAT}/winhttp_ws.cpp" "${PORT}/linux/xinput_sdl.cpp" "${PORT}/linux/hid_linux.cpp"
  "${PORT}/linux/full/audio_sdl.cpp" "${PORT}/linux/full/linux_media.cpp" "${PORT}/linux/full/gpu_features_linux.cpp"
  "${MAC}/renderer_metal.cpp" "${MAC}/gx_backend_mac.cpp" "${MAC}/pc_settings_metal.cpp" "${MAC}/settings_window_mac.cpp")
target_compile_definitions(mac_runtime PUBLIC MELEE_LINUX GX_PC_SETTINGS
  PRIVATE MELEE_PORT_VERSION="${MELEE_PORT_VERSION}" MELEE_PLAYBACK_GCT_BASE=0u MELEE_PLAYBACK_GCT_SIZE=0u
  GX_SHADER_CACHE_VERSION="mac-metal-1")
target_include_directories(mac_runtime PUBLIC "${COMPAT}" "${PORT}/runtime/ppc" "${PORT}/runtime/host" "${PORT}/runtime/hle"
  "${PORT}/runtime/gx" "${PORT}/runtime/mu_net" "${PORT}/runtime/abi" "${MELEE_PORT_GEN}" "${PROJECT_SOURCE_DIR}/native"
  "${PORT}/third_party" "${PORT}/third_party/onnxruntime/include" "${PORT}/linux" "${MAC}" "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_features(mac_runtime PUBLIC cxx_std_17)
target_compile_options(mac_runtime PRIVATE ${MELEE_MAC_COMMON_FLAGS} ${MELEE_HOST_ARCH})
# FFmpeg (build-deps.sh: the game's own formats only, static) for music packs, video backgrounds and pictures.
target_include_directories(mac_runtime PRIVATE "${MELEE_MAC_DEPS}/include" "${PORT}/app")
# The companion's pages (runtime/gx/companion.h): no second screen on a Mac, but MELEE_COMPANION_PREVIEW=1
# shows them beside the game for development.
target_compile_definitions(mac_runtime PUBLIC MELEE_COMPANION)
foreach(lib avformat avcodec swscale swresample avutil)
  target_link_libraries(mac_runtime PUBLIC "${MELEE_MAC_DEPS}/lib/lib${lib}.a")
endforeach()
target_link_libraries(mac_runtime PUBLIC z)
target_link_libraries(mac_runtime PUBLIC mac_native_animation mac_enet mac_monocypher mac_imgui mac_shaders mac_platform mac_embed
  port_disc_rvz mac_sdl2 mac_libusb curl "-framework Security" "-framework CoreGraphics" "-framework SystemConfiguration"
  Threads::Threads)

add_executable(MeleeUnlocked "${PORT}/app/main.cpp" "${PORT}/app/source_host.cpp" "${PORT}/app/source_guest_boundary.cpp"
  "${PORT}/app/source_p2p.cpp" "${NATIVE_RECORDING_CODES}")
target_include_directories(MeleeUnlocked PRIVATE "${CMAKE_CURRENT_BINARY_DIR}" "${PORT}/runtime/abi" "${PORT}/app")
target_compile_definitions(MeleeUnlocked PRIVATE MELEE_SOURCE_PORT MELEE_PORT_VERSION="${MELEE_PORT_VERSION}"
  MELEE_CPU_BASELINE_NAME="AVX2" MELEE_VERSION_COMMAS=${MELEE_VERSION_COMMAS} MELEE_GAME_LIBRARY_NAME="melee_game.so"
  MELEE_PLAYBACK_GCT_BASE=0u MELEE_PLAYBACK_GCT_SIZE=0u)
target_compile_options(MeleeUnlocked PRIVATE ${MELEE_MAC_COMMON_FLAGS})
# main.cpp checks the processor before anything else runs, so it carries no AVX2 code of its own.
if(MELEE_HOST_ARCH)
  set_source_files_properties("${PORT}/app/source_host.cpp" "${PORT}/app/source_guest_boundary.cpp" "${PORT}/app/source_p2p.cpp"
    PROPERTIES COMPILE_OPTIONS "${MELEE_HOST_ARCH}")
endif()
target_link_libraries(MeleeUnlocked PRIVATE mac_runtime)
if(NOT MELEE_MAC_ARM64)
  # x86-64: the game image and MEM1 sit at fixed addresses below 4 GB (elf_loader.cpp): a 64 KB page zero.
  target_link_options(MeleeUnlocked PRIVATE -Wl,-pagezero_size,0x10000)
endif()
set_target_properties(MeleeUnlocked PROPERTIES XCODE_ATTRIBUTE_ENABLE_HARDENED_RUNTIME NO)

# ---- the app bundle
if(MELEE_MAC_ARM64)
  set(MAC_PACKAGE_ARCH arm64)
else()
  set(MAC_PACKAGE_ARCH x86_64)
endif()
# The shader seed (make_shader_seed.py, run by build-metal.sh once the app exists): packaged when present.
set(MELEE_MAC_SHADER_SEED "${PROJECT_SOURCE_DIR}/build-mac-gen/shader-seed" CACHE PATH "make_shader_seed.py's --out folder")
add_custom_target(mac_app ALL
  COMMAND "${Python3_EXECUTABLE}" -B "${MAC}/package_metal.py" --host "$<TARGET_FILE:MeleeUnlocked>"
          --game "${MELEE_MAC_GAME_LIBRARY}" --arch ${MAC_PACKAGE_ARCH} --out "${CMAKE_BINARY_DIR}/MeleeUnlocked.app"
          --seed "${MELEE_MAC_SHADER_SEED}"
  DEPENDS MeleeUnlocked "${MAC}/package_metal.py" "${MAC}/game_container.py"
  COMMENT "Packaging MeleeUnlocked.app" VERBATIM)
