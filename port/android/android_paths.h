// Where the Android app keeps its parts. The program is libmain.so, loaded by the app's Java activity
// (MeleeActivity, an SDLActivity) from the package's native library folder, which also holds the game
// (libmelee_game.so: the only place an app may map code from). That folder is read-only and holds only
// libraries, so everything the other builds keep beside the program (Sys, Shaders, ui_sources, the
// game's sound ranges) is unpacked from the package's assets into the app's files folder, "resources"
// here. GetModuleFileName (port/linux/compat) names a program file in that folder, so every lookup
// beside the program finds them as on Linux.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <dlfcn.h>
#include <cstdlib>
#include <filesystem>

namespace android_host {
// The package's native library folder (libmain.so's own).
inline std::filesystem::path library_dir() {
  Dl_info info{};
  static const int here = 0;
  if (dladdr(&here, &info) && info.dli_fname) return std::filesystem::path(info.dli_fname).parent_path();
  return {};
}
// The unpacked assets (MeleeActivity sets MELEE_ANDROID_RESOURCES before the game starts).
inline std::filesystem::path resources() {
  const char* dir = std::getenv("MELEE_ANDROID_RESOURCES");
  return dir && *dir ? std::filesystem::path(dir) : library_dir();
}
// What GetModuleFileName(nullptr) answers: a program file inside the resources folder.
inline std::filesystem::path program_path() { return resources() / "MeleeUnlocked"; }
}  // namespace android_host
