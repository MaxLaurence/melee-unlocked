// See android_input.h.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "android_input.h"
#include <SDL.h>
#include <SDL_system.h>
#include <jni.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>

namespace host { void log(const char* fmt, ...); }

namespace {
// ---- MeleeActivity's methods, from any thread (SDL attaches it to the VM)
int activity_int(const char* method) {
  auto* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
  auto activity = static_cast<jobject>(SDL_AndroidGetActivity());
  if (!env || !activity) return -1;
  int result = -1;
  jclass cls = env->GetObjectClass(activity);
  if (jmethodID m = env->GetMethodID(cls, method, "()I")) result = env->CallIntMethod(activity, m);
  if (env->ExceptionCheck()) { env->ExceptionClear(); result = -1; }
  env->DeleteLocalRef(cls);
  env->DeleteLocalRef(activity);
  return result;
}

// ---- built-in sticks
int64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
std::atomic<uint32_t> g_builtin_pad{0};   // vendor << 16 | product of the handheld's own pad; 0: none known
std::atomic<float> g_raw[4];              // left x, y, right x, y: -1..1, right and down positive
std::atomic<int64_t> g_raw_at{0};         // when the reader last delivered

// slippi-android's StickCalibration: re-centre, the per-direction outer scale (only for unclipped raw
// values), a radial deadzone, a power response curve and an output cap, keeping the direction.
using Calibration = android_host::StickCalibration;
void apply(const Calibration& c, float x, float y, bool outer_scale, float& ox, float& oy) {
  float dx = x - c.center_x, dy = y - c.center_y;
  if (outer_scale) { dx *= dx >= 0 ? c.scale_x_pos : c.scale_x_neg; dy *= dy >= 0 ? c.scale_y_pos : c.scale_y_neg; }
  const float mag = std::sqrt(dx * dx + dy * dy);
  if (mag < c.deadzone || mag <= 0) { ox = oy = 0; return; }
  float scaled = std::min(1.0f, (mag - c.deadzone) / (1.0f - c.deadzone));
  if (c.curve > 1.0f) scaled = std::pow(scaled, c.curve);
  scaled *= c.cap;
  ox = dx / mag * scaled; oy = dy / mag * scaled;
}
// Its defaults for a reader that is still clipped by the firmware (the Thor's: about a quarter of the
// nominal range): four times outward. Main stick: 2% deadzone, curve 1.5; C-stick: 10%, curve 2.0 (smashes
// only, so a light flick must not smash). builtin-sticks.ini in the player's folder overrides them
// (the companion's Sticks page writes it):
//   main deadzone=0.02 curve=1.5 scale=4 cap=1 center_x=0 center_y=0
//   c deadzone=0.10 curve=2.0 scale=4 cap=1
// and per direction: scale_x_pos= scale_x_neg= scale_y_pos= scale_y_neg=
struct Sticks { Calibration main, c; };
Sticks defaults() {
  Sticks s;
  for (Calibration* cal : {&s.main, &s.c}) cal->scale_x_pos = cal->scale_x_neg = cal->scale_y_pos = cal->scale_y_neg = 4.0f;
  s.c.deadzone = 0.10f; s.c.curve = 2.0f;
  return s;
}
std::mutex g_sticks_mutex;
bool g_sticks_loaded = false;
Sticks g_sticks;
const char* kIni = "builtin-sticks.ini";   // the player's folder is the working folder (android_launch.h)
Sticks sticks() {
  std::lock_guard<std::mutex> lock(g_sticks_mutex);
  if (g_sticks_loaded) return g_sticks;
  g_sticks_loaded = true;
  Sticks s = defaults();
  std::ifstream in(kIni);
  std::string line;
  while (std::getline(in, line)) {
    std::istringstream words(line);
    std::string which, pair;
    words >> which;
    Calibration* cal = which == "main" ? &s.main : which == "c" ? &s.c : nullptr;
    if (!cal) continue;
    while (words >> pair) {
      const auto eq = pair.find('=');
      if (eq == std::string::npos) continue;
      const std::string key = pair.substr(0, eq);
      const float v = std::strtof(pair.c_str() + eq + 1, nullptr);
      if (key == "deadzone") cal->deadzone = std::clamp(v, 0.0f, 0.5f);
      else if (key == "curve") cal->curve = std::clamp(v, 1.0f, 10.0f);
      else if (key == "cap") cal->cap = std::clamp(v, 0.3f, 1.0f);
      else if (key == "scale") cal->scale_x_pos = cal->scale_x_neg = cal->scale_y_pos = cal->scale_y_neg = std::clamp(v, 0.25f, 8.0f);
      else if (key == "scale_x_pos") cal->scale_x_pos = std::clamp(v, 0.25f, 8.0f);
      else if (key == "scale_x_neg") cal->scale_x_neg = std::clamp(v, 0.25f, 8.0f);
      else if (key == "scale_y_pos") cal->scale_y_pos = std::clamp(v, 0.25f, 8.0f);
      else if (key == "scale_y_neg") cal->scale_y_neg = std::clamp(v, 0.25f, 8.0f);
      else if (key == "center_x") cal->center_x = std::clamp(v, -0.5f, 0.5f);
      else if (key == "center_y") cal->center_y = std::clamp(v, -0.5f, 0.5f);
    }
  }
  host::log("built-in sticks: main deadzone %.2f curve %.1f scale %.1f cap %.2f; C deadzone %.2f curve %.1f scale %.1f cap %.2f%s",
            s.main.deadzone, s.main.curve, s.main.scale_x_pos, s.main.cap, s.c.deadzone, s.c.curve, s.c.scale_x_pos, s.c.cap,
            in.is_open() ? " (builtin-sticks.ini)" : " (defaults)");
  g_sticks = s;
  return s;
}
int16_t to_xinput(float v) { return (int16_t)std::clamp((int)std::lround(v * 32767.0f), -32768, 32767); }
}  // namespace

namespace android_host {

int gc_adapter_open() { return activity_int("gcAdapterOpen"); }
void gc_adapter_close() { activity_int("gcAdapterClose"); }

bool builtin_sticks(uint16_t vendor, uint16_t product, int16_t& lx, int16_t& ly, int16_t& rx, int16_t& ry) {
  const uint32_t id = g_builtin_pad.load(std::memory_order_relaxed);
  if (!id || id != ((uint32_t)vendor << 16 | product)) return false;
  const bool raw = now_ns() - g_raw_at.load(std::memory_order_acquire) < 100'000'000;   // delivered in the last 100 ms
  float v[4];
  if (raw) for (int i = 0; i < 4; ++i) v[i] = g_raw[i].load(std::memory_order_relaxed);
  else {   // SDL's (Android's) values, already scaled by the system: calibrated without the outer scale
    v[0] = lx / 32767.0f; v[1] = -ly / 32767.0f; v[2] = rx / 32767.0f; v[3] = -ry / 32767.0f;
  }
  static std::atomic<int> logged{-1};
  if (logged.exchange(raw ? 1 : 0) != (raw ? 1 : 0))
    host::log("built-in sticks (pad %04X:%04X): %s", vendor, product,
              raw ? "unclipped values from the AYN mapping service" : "Android's values (no raw reader)");
  const Sticks s = sticks();
  float mx, my, cx, cy;
  apply(s.main, v[0], v[1], raw, mx, my);
  apply(s.c, v[2], v[3], raw, cx, cy);
  lx = to_xinput(mx); ly = to_xinput(-my); rx = to_xinput(cx); ry = to_xinput(-cy);   // XInput's Y grows upward
  return true;
}

bool builtin_pad_present() { return g_builtin_pad.load(std::memory_order_relaxed) != 0; }

bool builtin_raw(float out[4]) {
  const bool fresh = now_ns() - g_raw_at.load(std::memory_order_acquire) < 100'000'000;
  for (int i = 0; i < 4; ++i) out[i] = g_raw[i].load(std::memory_order_relaxed);
  return fresh;
}

void builtin_calibration(StickCalibration& main, StickCalibration& c) {
  const Sticks s = sticks();
  main = s.main; c = s.c;
}

void set_builtin_calibration(const StickCalibration& main, const StickCalibration& c) {
  sticks();   // loaded first, so a later first read does not replace these with the file's
  std::lock_guard<std::mutex> lock(g_sticks_mutex);
  g_sticks.main = main; g_sticks.c = c;
}

void default_builtin_calibration(StickCalibration& main, StickCalibration& c) {
  const Sticks s = defaults();
  main = s.main; c = s.c;
}

bool save_builtin_calibration() {
  const Sticks s = sticks();
  const std::string partial = std::string(kIni) + ".partial";
  {
    std::ofstream out(partial, std::ios::trunc);
    if (!out) return false;
    auto line = [&](const char* which, const Calibration& c) {
      out << which << " deadzone=" << c.deadzone << " curve=" << c.curve << " cap=" << c.cap << " center_x=" << c.center_x
          << " center_y=" << c.center_y << " scale_x_pos=" << c.scale_x_pos << " scale_x_neg=" << c.scale_x_neg
          << " scale_y_pos=" << c.scale_y_pos << " scale_y_neg=" << c.scale_y_neg << "\n";
    };
    out << "# The AYN handheld's built-in sticks (Melee Unlocked's companion, Sticks page).\n";
    line("main", s.main);
    line("c", s.c);
    if (!out.good()) return false;
  }
  if (std::rename(partial.c_str(), kIni) != 0) return false;
  host::log("built-in sticks: calibration saved to %s", kIni);
  return true;
}

void calibrate_stick(const StickCalibration& cal, float x, float y, bool raw, float& out_x, float& out_y) { apply(cal, x, y, raw, out_x, out_y); }

}  // namespace android_host

// ---- called by MeleeActivity
// (No host::log here: these run on Android's threads before the game has read its options, and the log
// file is opened by the first line logged, so an early line would open it in the wrong place for good.)
extern "C" JNIEXPORT void JNICALL Java_io_github_meleeunlocked_MeleeActivity_nativeBuiltinPad(JNIEnv*, jclass, jint vendor, jint product) {
  g_builtin_pad.store(vendor > 0 ? ((uint32_t)vendor << 16 | (uint32_t)(product & 0xFFFF)) : 0);
}
extern "C" JNIEXPORT void JNICALL Java_io_github_meleeunlocked_MeleeActivity_nativeBuiltinSticks(JNIEnv*, jclass, jfloat lx, jfloat ly,
                                                                                                 jfloat rx, jfloat ry) {
  g_raw[0].store(lx, std::memory_order_relaxed); g_raw[1].store(ly, std::memory_order_relaxed);
  g_raw[2].store(rx, std::memory_order_relaxed); g_raw[3].store(ry, std::memory_order_relaxed);
  g_raw_at.store(now_ns(), std::memory_order_release);
}
