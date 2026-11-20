// See android_second_screen.h.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "android_second_screen.h"
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <SDL.h>
#include <SDL_system.h>
#include <jni.h>
#include <chrono>
#include <condition_variable>
#include <mutex>

namespace {
std::mutex g_mutex;
std::condition_variable g_released_cv;
android_host::SecondSurface g_surface;   // what Android gives now (Java's own reference on the window)
uint64_t g_next_generation = 1;
uint64_t g_using = 0;                    // the generation the renderer holds (0: none)
std::vector<android_host::Touch> g_touches;
}  // namespace

namespace android_host {

uint64_t second_screen_generation() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_surface.generation;
}

SecondSurface second_screen_acquire() {
  std::lock_guard<std::mutex> lock(g_mutex);
  SecondSurface s = g_surface;
  if (s.window) {
    ANativeWindow_acquire(s.window);
    g_using = s.generation;
  }
  return s;
}

void second_screen_release(SecondSurface& surface) {
  if (surface.window) ANativeWindow_release(surface.window);
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (surface.generation && g_using == surface.generation) g_using = 0;
  }
  surface = SecondSurface{};
  g_released_cv.notify_all();
}

bool second_screen_present() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_surface.window != nullptr;
}

std::vector<Touch> second_screen_touches() {
  std::lock_guard<std::mutex> lock(g_mutex);
  std::vector<Touch> out;
  out.swap(g_touches);
  return out;
}

// ---- MeleeActivity's methods (SDL attaches the calling thread to the VM)
namespace {
template <typename Call>
auto with_activity(Call call, decltype(call(nullptr, nullptr, nullptr)) fallback) {
  auto* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
  auto activity = static_cast<jobject>(SDL_AndroidGetActivity());
  if (!env || !activity) return fallback;
  jclass cls = env->GetObjectClass(activity);
  auto result = call(env, activity, cls);
  if (env->ExceptionCheck()) { env->ExceptionClear(); result = fallback; }
  env->DeleteLocalRef(cls);
  env->DeleteLocalRef(activity);
  return result;
}
}  // namespace

bool viewer_launch(const std::vector<std::string>& args) {
  return with_activity([&](JNIEnv* env, jobject activity, jclass cls) -> bool {
    jmethodID m = env->GetMethodID(cls, "launchViewer", "([Ljava/lang/String;)Z");
    if (!m) return false;
    jclass string_class = env->FindClass("java/lang/String");
    jobjectArray array = env->NewObjectArray((jsize)args.size(), string_class, nullptr);
    for (size_t i = 0; i < args.size(); ++i) {
      jstring a = env->NewStringUTF(args[i].c_str());
      env->SetObjectArrayElement(array, (jsize)i, a);
      env->DeleteLocalRef(a);
    }
    const bool ok = env->CallBooleanMethod(activity, m, array);
    env->DeleteLocalRef(array);
    env->DeleteLocalRef(string_class);
    return ok;
  }, false);
}

bool viewer_running() {
  return with_activity([](JNIEnv* env, jobject activity, jclass cls) -> bool {
    jmethodID m = env->GetMethodID(cls, "viewerRunning", "()Z");
    return m && env->CallBooleanMethod(activity, m);
  }, false);
}

void viewer_close() {
  with_activity([](JNIEnv* env, jobject activity, jclass cls) -> bool {
    if (jmethodID m = env->GetMethodID(cls, "closeViewer", "()V")) env->CallVoidMethod(activity, m);
    return true;
  }, false);
}

bool relaunch(const std::string& args, bool come_back) {
  return with_activity([&](JNIEnv* env, jobject activity, jclass cls) -> bool {
    jmethodID m = env->GetMethodID(cls, "requestRelaunch", "(Ljava/lang/String;Z)Z");
    if (!m) return false;
    jstring a = env->NewStringUTF(args.c_str());
    const bool ok = env->CallBooleanMethod(activity, m, a, (jboolean)come_back);
    env->DeleteLocalRef(a);
    return ok;
  }, false);
}

bool second_display() {
  return with_activity([](JNIEnv* env, jobject activity, jclass cls) -> bool {
    jmethodID m = env->GetMethodID(cls, "hasBottomDisplay", "()Z");
    return m && env->CallBooleanMethod(activity, m);
  }, false);
}

void before_process_end() {
  with_activity([](JNIEnv* env, jobject activity, jclass cls) -> bool {
    if (jmethodID m = env->GetMethodID(cls, "gameEnding", "()V")) env->CallVoidMethod(activity, m);
    return true;
  }, false);
}

}  // namespace android_host

extern "C" JNIEXPORT void JNICALL Java_io_github_meleeunlocked_BottomScreen_nativeSurface(JNIEnv* env, jclass, jobject surface,
                                                                                                   jint width, jint height, jfloat density) {
  ANativeWindow* window = surface ? ANativeWindow_fromSurface(env, surface) : nullptr;
  ANativeWindow* old = nullptr;
  {
    std::unique_lock<std::mutex> lock(g_mutex);
    const uint64_t previous = g_surface.generation;
    old = g_surface.window;
    g_surface.window = window;
    g_surface.width = width;
    g_surface.height = height;
    g_surface.density = density > 0 ? density : 1.0f;
    g_surface.generation = window ? g_next_generation++ : 0;
    g_touches.clear();
    // Going away: Android may destroy the surface as soon as this returns, so the renderer lets go
    // first. Bounded, so a stuck renderer cannot freeze Android's UI thread (its own reference keeps
    // the window object valid; Vulkan then reports the surface lost).
    if (!window && previous && g_using == previous) {
      g_released_cv.wait_for(lock, std::chrono::milliseconds(1500), [&] { return g_using != previous; });
      if (g_using == previous)
        __android_log_print(ANDROID_LOG_WARN, "MeleeUnlocked", "second screen: the renderer did not let go of the surface in time");
    }
  }
  if (old) ANativeWindow_release(old);
  __android_log_print(ANDROID_LOG_INFO, "MeleeUnlocked", "second screen: surface %s (%dx%d, density %.2f)",
                      window ? "ready" : "gone", (int)width, (int)height, (double)density);
}

extern "C" JNIEXPORT void JNICALL Java_io_github_meleeunlocked_BottomScreen_nativeTouch(JNIEnv*, jclass, jint action, jint pointer,
                                                                                                 jfloat x, jfloat y) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_touches.size() < 512) g_touches.push_back({(int)action, (int)pointer, (float)x, (float)y});
}
