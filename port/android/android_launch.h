// The Android app's launch: MeleeActivity (port/android/java) starts SDL_main (app/main.cpp) with
// --android-files <the app's files folder>, the disc it was given (--iso) and anything a test passed
// in the intent. This adds the defaults the other builds take from beside the program or the home
// folder: the player's folder (settings, saves, replays, logs, the shader cache) under the app's
// files folder, the unpacked assets (Sys, Shaders, ui_sources) as the program's own folder, and the
// game's standard output and errors in logcat (tag MeleeUnlocked).
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "android_paths.h"
#include <android/log.h>
#include <pthread.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace android_host {
// stdout and stderr into logcat, line by line, on a thread of their own.
inline void log_output_to_logcat() {
  static int pipe_fds[2] = {-1, -1};
  if (pipe_fds[0] >= 0 || pipe(pipe_fds) != 0) return;
  std::setvbuf(stdout, nullptr, _IOLBF, 0);
  std::setvbuf(stderr, nullptr, _IONBF, 0);
  dup2(pipe_fds[1], STDOUT_FILENO);
  dup2(pipe_fds[1], STDERR_FILENO);
  pthread_t thread;
  pthread_create(&thread, nullptr, [](void*) -> void* {
    pthread_setname_np(pthread_self(), "stdout-logcat");
    std::string line;
    char buffer[1024];
    for (;;) {
      const ssize_t n = read(pipe_fds[0], buffer, sizeof buffer);
      if (n <= 0) break;
      for (ssize_t i = 0; i < n; ++i) {
        if (buffer[i] == '\n') { __android_log_write(ANDROID_LOG_INFO, "MeleeUnlocked", line.c_str()); line.clear(); }
        else line += buffer[i];
      }
    }
    return nullptr;
  }, nullptr);
  pthread_detach(thread);
}

inline std::vector<std::string> launch_arguments(int argc, char** argv) {
  namespace fs = std::filesystem;
  log_output_to_logcat();
  std::vector<std::string> args;
  fs::path files;
  for (int i = 0; i < argc; ++i) {
    const std::string a = argv[i] ? argv[i] : "";
    if (a == "--android-files" && i + 1 < argc) { files = argv[++i]; continue; }
    args.push_back(a);
  }
  if (args.empty()) args.push_back("MeleeUnlocked");
  if (files.empty()) files = "/data/local/tmp/melee-unlocked";
  const fs::path res = files / "res";
  setenv("MELEE_ANDROID_RESOURCES", res.c_str(), 1);
  setenv("HOME", files.c_str(), 0);
  auto value = [&](const std::string& name) -> std::string {
    for (size_t i = 1; i + 1 < args.size(); ++i) if (args[i] == name) return args[i + 1];
    return {};
  };
  fs::path user = value("--user-dir");
  if (user.empty()) user = files / "User/Slippi";
  auto default_path = [&](const std::string& flag, const fs::path& path) {
    if (value(flag).empty()) { args.push_back(flag); args.push_back(path.string()); }
  };
  default_path("--user-dir", user);
  default_path("--settings-path", user / "port-settings.ini");
  default_path("--card-dir", user / "GC/CardA");
  default_path("--replay-dir", user / "Replays");
  default_path("--log-file", user / "melee_port.log");
  default_path("--shader-cache", user / "shaders");
  default_path("--sys-dir", res / "Sys");
  std::error_code ec;
  fs::create_directories(user, ec);
  for (const auto& flag : {"--card-dir", "--replay-dir", "--shader-cache"}) fs::create_directories(value(flag), ec);
  for (const auto& flag : {"--settings-path", "--log-file"}) fs::create_directories(fs::path(value(flag)).parent_path(), ec);
  fs::create_directories(user / "tmp", ec);
  setenv("TMPDIR", (user / "tmp").c_str(), 1);
  fs::current_path(user, ec);
  std::string line = "launch:";
  for (const auto& a : args) line += " " + a;
  __android_log_write(ANDROID_LOG_INFO, "MeleeUnlocked", line.c_str());
  return args;
}
}  // namespace android_host
