// See main_thread.h.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "main_thread.h"
#include <SDL.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>

namespace mac_host {
namespace {
std::atomic<bool> g_active{false};
std::atomic<bool> g_game_done{false};
// Work for the main thread, run by its own loop below and never inside a block on the main dispatch
// queue: that queue is serial, and AppKit finishes a full screen transition (among other things)
// with blocks of its own on it, which a render loop running in one would hold back for good.
std::mutex g_tasks_mutex;
std::deque<std::shared_ptr<std::packaged_task<void()>>> g_tasks;
std::shared_ptr<std::packaged_task<void()>> take_task() {
  std::lock_guard<std::mutex> lock(g_tasks_mutex);
  if (g_tasks.empty()) return nullptr;
  auto task = std::move(g_tasks.front());
  g_tasks.pop_front();
  return task;
}
// Makes the main thread's CFRunLoopRunInMode below return (it returns after one source is handled).
void wake_main() {
  dispatch_async(dispatch_get_main_queue(), ^{});
  CFRunLoopWakeUp(CFRunLoopGetMain());
}

struct GameStart {
  int (*game)(int, char**);
  int argc;
  char** argv;
  int code = 0;
};

void* game_main(void* p) {
  auto* start = static_cast<GameStart*>(p);
  pthread_setname_np("game");
  start->code = start->game(start->argc, start->argv);
  g_game_done.store(true);
  wake_main();   // so the main thread sees the game has returned
  return nullptr;
}
}  // namespace

bool wants_game_thread(const std::vector<std::string>& args) {
  if (const char* one = std::getenv("MELEE_MAC_ONE_THREAD"); one && std::strcmp(one, "1") == 0) return false;
  for (size_t i = 1; i < args.size(); ++i) {
    const std::string& a = args[i];
    // No window (or no game): everything stays on the main thread.
    if (a == "--hidden" || a == "--headless" || a == "--settings-window" || a == "--version" || a == "--help" ||
        a == "-h" || a == "--card-self-test" || a == "--list-audio-devices")
      return false;
  }
  return true;
}

bool game_thread_active() { return g_active.load(std::memory_order_acquire); }

int run_game_on_own_thread(int (*game)(int, char**), int argc, char** argv) {
  // The controller subsystem starts here, on the main thread: SDL schedules macOS's HID device
  // callbacks on the run loop of the thread that starts it, and the main thread's events (window.cpp)
  // run it. The hints are window_create's (no HIDAPI GameCube driver: the adapter is the game's).
  SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
  SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_GAMECUBE, "0");
  SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS);

  GameStart start{game, argc, argv};
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, size_t(64) << 20);
  pthread_attr_set_qos_class_np(&attr, QOS_CLASS_USER_INTERACTIVE, 0);
  pthread_t thread;
  g_active.store(true, std::memory_order_release);
  const int rc = pthread_create(&thread, &attr, game_main, &start);
  pthread_attr_destroy(&attr);
  if (rc != 0) {
    g_active.store(false, std::memory_order_release);
    return game(argc, argv);
  }
  // The render loop arrives as a task (MainThreadTask) and runs here, at the top of the main thread.
  while (!g_game_done.load()) {
    if (auto task = take_task()) { (*task)(); continue; }
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.25, true);
  }
  while (auto task = take_task()) (*task)();   // one posted as the game returned
  pthread_join(thread, nullptr);
  return start.code;
}

void post_to_main(std::shared_ptr<std::packaged_task<void()>> task) {
  {
    std::lock_guard<std::mutex> lock(g_tasks_mutex);
    g_tasks.push_back(std::move(task));
  }
  wake_main();
}

}  // namespace mac_host
