# The Android Source Port: the full host (Slippi online, replays, mods, the settings panel) as libmain.so,
# which SDL's Java activity loads, drawn with the Vulkan renderer (runtime/gx/gx_vulkan.cpp). arm64-v8a.
#
# The game is the GCC-built Linux AArch64 ELF (port/android/build-game.sh: the arm64 Mac's build with its
# address window at 256 GB), loaded by the host itself (port/mac/elf_loader.cpp) from the package's
# native library folder, where it is installed as libmelee_game.so.
#
# port/android/build-apk.sh does everything (dependencies, game library, translation headers, this, the APK).
cmake_minimum_required(VERSION 3.22)
enable_language(C)

set(MELEE_PORT_GEN "" CACHE PATH "The guest translation's generated headers (hle_decls.h, guest_symbols.h)")
set(MELEE_ANDROID_DEPS "" CACHE PATH "Install prefix of SDL2, glslang, FFmpeg, OpenSSL and curl (port/android/build-deps.sh)")
foreach(needed "${MELEE_PORT_GEN}/hle_decls.h" "${MELEE_PORT_GEN}/guest_symbols.h" "${MELEE_ANDROID_DEPS}/lib/libSDL2.so"
               "${MELEE_ANDROID_DEPS}/lib/libglslang.a" "${MELEE_ANDROID_DEPS}/lib/libcurl.a" "${MELEE_ANDROID_DEPS}/lib/libavformat.a")
  if(NOT EXISTS "${needed}")
    message(FATAL_ERROR "Missing build input: ${needed}")
  endif()
endforeach()

set(PORT "${PROJECT_SOURCE_DIR}/port")
set(COMPAT "${PORT}/linux/compat")
set(ANDROID_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(DEPS "${MELEE_ANDROID_DEPS}")
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

set(MELEE_ANDROID_COMMON_FLAGS -include "${COMPAT}/windows.h" -ffp-contract=off -fno-strict-aliasing
    -Wno-unknown-pragmas -Wno-multichar -Wno-deprecated-declarations -Wno-everything -pthread)

# ---- generated sources, as the other builds make them
set(ANDROID_FOBJ "${CMAKE_CURRENT_BINARY_DIR}/generated/FObjHost.cpp")
add_custom_command(OUTPUT "${ANDROID_FOBJ}"
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/generate_fobj_host.py" "${ANDROID_FOBJ}"
  DEPENDS "${PROJECT_SOURCE_DIR}/tools/generate_fobj_host.py" VERBATIM)
add_library(android_native_animation STATIC "${ANDROID_FOBJ}")
target_include_directories(android_native_animation PUBLIC "${PROJECT_SOURCE_DIR}/native")
target_compile_features(android_native_animation PUBLIC cxx_std_14)
target_compile_options(android_native_animation PRIVATE -ffp-contract=off -w)
set(NATIVE_RECORDING_CODES "${CMAKE_CURRENT_BINARY_DIR}/native_recording_codes.h")
add_custom_command(OUTPUT "${NATIVE_RECORDING_CODES}"
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/native_recording_codes.py" "${NATIVE_RECORDING_CODES}"
  DEPENDS "${PROJECT_SOURCE_DIR}/tools/native_recording_codes.py" "${PORT}/recomp/gecko.py"
          "${PORT}/slippi_sys_general/GameSettings/GALE01r2.ini" "${PORT}/slippi_sys/GameSettings/GALE01r2.ini"
  VERBATIM)

# ---- third-party libraries
file(GLOB ANDROID_ENET_SOURCES "${PORT}/third_party/enet/*.c")
add_library(android_enet STATIC ${ANDROID_ENET_SOURCES})
target_include_directories(android_enet PUBLIC "${PORT}/third_party/enet/include")
target_compile_definitions(android_enet PRIVATE HAS_SOCKLEN_T=1 HAS_POLL=1 HAS_FCNTL=1 HAS_MSGHDR_FLAGS=1 HAS_GETADDRINFO=1
  HAS_GETNAMEINFO=1 HAS_INET_PTON=1 HAS_INET_NTOP=1)
target_compile_options(android_enet PRIVATE -w)
add_library(android_monocypher STATIC "${PORT}/third_party/monocypher/monocypher.c" "${PORT}/third_party/monocypher/monocypher-ed25519.c")
target_include_directories(android_monocypher PUBLIC "${PORT}/third_party/monocypher")

add_library(android_sdl2 SHARED IMPORTED)
set_target_properties(android_sdl2 PROPERTIES IMPORTED_LOCATION "${DEPS}/lib/libSDL2.so"
  INTERFACE_INCLUDE_DIRECTORIES "${DEPS}/include;${DEPS}/include/SDL2")

add_library(android_imgui STATIC
  "${PORT}/third_party/imgui/imgui.cpp" "${PORT}/third_party/imgui/imgui_draw.cpp"
  "${PORT}/third_party/imgui/imgui_tables.cpp" "${PORT}/third_party/imgui/imgui_widgets.cpp"
  "${PORT}/third_party/imgui/backends/imgui_impl_sdl2.cpp" "${PORT}/third_party/imgui/backends/imgui_impl_vulkan.cpp")
# The Vulkan renderer opens libvulkan at run time: headers only.
target_include_directories(android_imgui PUBLIC "${PORT}/third_party/imgui" "${PORT}/third_party/imgui/backends" "${PORT}/third_party/vulkan-headers/include")
target_compile_definitions(android_imgui PUBLIC IMGUI_IMPL_VULKAN_NO_PROTOTYPES)
target_compile_features(android_imgui PUBLIC cxx_std_17)
target_link_libraries(android_imgui PUBLIC android_sdl2 ${CMAKE_DL_LIBS})

# HLSL to SPIR-V in the program (shader_spirv.cpp): glslang with SPIRV-Tools, built by build-deps.sh.
add_library(android_shaders STATIC "${ANDROID_DIR}/shader_spirv.cpp")
target_include_directories(android_shaders PRIVATE "${DEPS}/include")
target_compile_definitions(android_shaders PRIVATE ENABLE_HLSL)
target_compile_features(android_shaders PUBLIC cxx_std_17)
foreach(lib glslang MachineIndependent GenericCodeGen OSDependent SPIRV glslang-default-resource-limits SPIRV-Tools-opt SPIRV-Tools)
  target_link_libraries(android_shaders PRIVATE "${DEPS}/lib/lib${lib}.a")
endforeach()

# libusb (the vendored 1.0.27, its Linux usbfs backend) for the GameCube adapter, opened through Android's
# USB host API (libusb_wrap_sys_device).
set(LIBUSB "${PORT}/third_party/libusb")
add_library(android_libusb STATIC "${LIBUSB}/libusb/core.c" "${LIBUSB}/libusb/descriptor.c" "${LIBUSB}/libusb/hotplug.c"
  "${LIBUSB}/libusb/io.c" "${LIBUSB}/libusb/strerror.c" "${LIBUSB}/libusb/sync.c" "${LIBUSB}/libusb/os/linux_usbfs.c"
  "${LIBUSB}/libusb/os/linux_netlink.c" "${LIBUSB}/libusb/os/events_posix.c" "${LIBUSB}/libusb/os/threads_posix.c")
target_include_directories(android_libusb PRIVATE "${ANDROID_DIR}/libusb" PUBLIC "${LIBUSB}/libusb")
target_compile_options(android_libusb PRIVATE -w)
target_link_libraries(android_libusb PUBLIC log)

include("${PORT}/app/disc_rvz.cmake")

# ---- the host runtime: every portable file of runtime/, the Windows renderers and GPU libraries left out,
# the Linux implementations, the game loader and the Vulkan factory added.
file(GLOB ANDROID_RUNTIME_SOURCES "${PORT}/runtime/ppc/*.cpp" "${PORT}/runtime/hle/*.cpp" "${PORT}/runtime/host/*.cpp"
  "${PORT}/runtime/gx/*.cpp" "${PORT}/runtime/mu_net/*.cpp")
foreach(removed
    runtime/ppc/leaf_stencils.cpp runtime/ppc/leaf_translator.cpp runtime/ppc/ram_translator.cpp
    runtime/gx/gx_d3d11.cpp runtime/gx/gx_d3d12.cpp runtime/gx/gx_dlss5.cpp runtime/gx/gx_dlss5_scaling.cpp
    runtime/gx/gx_dxr_gpu_scene.cpp runtime/gx/gx_dxr_path_tracer.cpp runtime/gx/gx_streamline.cpp runtime/gx/gx_xess.cpp
    runtime/gx/gx_fsr.cpp runtime/gx/gx_fsr_vk.cpp runtime/gx/rtx_scene.cpp runtime/gx/rtx_tracer.cpp
    runtime/gx/rtx_melee_integrator.cpp runtime/gx/gx_backend.cpp runtime/gx/pc_settings_dx11.cpp runtime/host/audio.cpp
    runtime/gx/gx_dxc_linux.cpp)
  list(REMOVE_ITEM ANDROID_RUNTIME_SOURCES "${PORT}/${removed}")
endforeach()
add_library(android_runtime STATIC ${ANDROID_RUNTIME_SOURCES} "${PORT}/third_party/slippilib/SlippiGame.cpp"
  "${COMPAT}/compat.cpp" "${COMPAT}/winhttp_ws.cpp" "${PORT}/linux/xinput_sdl.cpp" "${PORT}/linux/hid_linux.cpp"
  "${PORT}/linux/full/audio_sdl.cpp" "${PORT}/linux/full/linux_media.cpp" "${PORT}/linux/full/gpu_features_linux.cpp"
  "${ANDROID_DIR}/gx_backend_android.cpp" "${ANDROID_DIR}/android_input.cpp")
target_compile_definitions(android_runtime PUBLIC MELEE_LINUX GX_PC_SETTINGS
  PRIVATE MELEE_PORT_VERSION="${MELEE_PORT_VERSION}" MELEE_PLAYBACK_GCT_BASE=0u MELEE_PLAYBACK_GCT_SIZE=0u
  GX_SHADER_CACHE_VERSION="android-vulkan-1")
target_include_directories(android_runtime PUBLIC "${COMPAT}" "${PORT}/runtime/ppc" "${PORT}/runtime/host" "${PORT}/runtime/hle"
  "${PORT}/runtime/gx" "${PORT}/runtime/mu_net" "${PORT}/runtime/abi" "${MELEE_PORT_GEN}" "${PROJECT_SOURCE_DIR}/native"
  "${PORT}/third_party" "${PORT}/third_party/onnxruntime/include" "${PORT}/linux" "${ANDROID_DIR}" "${CMAKE_CURRENT_BINARY_DIR}"
  "${DEPS}/include")
target_compile_features(android_runtime PUBLIC cxx_std_17)
# SDL's main macro stays out of the host's own code (main.cpp defines SDL_main itself), and the
# <execinfo.h> shim (port/android/shim) comes before the NDK's for Android versions before 13.
target_compile_definitions(android_runtime PUBLIC SDL_MAIN_HANDLED)
target_include_directories(android_runtime BEFORE PUBLIC "${ANDROID_DIR}/shim")
target_compile_options(android_runtime PRIVATE ${MELEE_ANDROID_COMMON_FLAGS})
foreach(lib avformat avcodec swscale swresample avutil curl ssl crypto)
  target_link_libraries(android_runtime PUBLIC "${DEPS}/lib/lib${lib}.a")
endforeach()
target_link_libraries(android_runtime PUBLIC android_native_animation android_enet android_monocypher android_imgui android_shaders
  port_disc_rvz android_sdl2 android_libusb z log android Threads::Threads ${CMAKE_DL_LIBS} m)

# The game loader (port/mac/elf_loader.cpp): plain C++, without the Win32 compatibility headers.
add_library(android_loader STATIC "${PORT}/mac/elf_loader.cpp")
target_include_directories(android_loader PRIVATE "${PORT}/runtime/abi" "${PORT}/mac")
target_compile_features(android_loader PRIVATE cxx_std_17)

add_library(main SHARED "${PORT}/app/main.cpp" "${PORT}/app/source_host.cpp" "${PORT}/app/source_guest_boundary.cpp"
  "${PORT}/app/source_p2p.cpp" "${NATIVE_RECORDING_CODES}")
target_include_directories(main PRIVATE "${CMAKE_CURRENT_BINARY_DIR}" "${PORT}/runtime/abi" "${PORT}/app")
target_compile_definitions(main PRIVATE MELEE_SOURCE_PORT MELEE_PORT_VERSION="${MELEE_PORT_VERSION}"
  MELEE_CPU_BASELINE_NAME="ARM64" MELEE_VERSION_COMMAS=${MELEE_VERSION_COMMAS} MELEE_GAME_LIBRARY_NAME="libmelee_game.so"
  MELEE_PLAYBACK_GCT_BASE=0u MELEE_PLAYBACK_GCT_SIZE=0u)
target_compile_options(main PRIVATE ${MELEE_ANDROID_COMMON_FLAGS})
# SDL's Java glue calls into SDL2 and finds SDL_main by name: keep every symbol of the runtime's
# static libraries the host refers to, and the entry point exported.
target_link_libraries(main PRIVATE android_runtime android_loader)
target_link_options(main PRIVATE -Wl,--no-undefined -Wl,-z,max-page-size=16384 -Wl,--no-dependent-libraries -Wl,-Bsymbolic)  # the Windows #pragma comment(lib) lines; FFmpeg's NEON code binds its own tables
