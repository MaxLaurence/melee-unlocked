// The game and the render loop on a Mac. AppKit takes windows and their events only on the main
// thread, while the render thread (gx/threaded_backend.cpp) owns the window on Windows and Linux. So a
// launch with a window runs the game on a thread of its own and the render loop on the main thread:
// the same two threads with the same jobs, the other way round. A hidden or headless launch keeps
// everything on the main thread, as before.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <vector>

namespace mac_host {

// Whether this launch gets a window and so runs the game on its own thread. MELEE_MAC_ONE_THREAD=1
// keeps the old single thread (the render thread is then off, so the frame rate is 60).
bool wants_game_thread(const std::vector<std::string>& args);

// Runs game(argc, argv) on a new thread (64 MB of stack, user-interactive priority) and serves the
// main thread's run loop, including the render loop, until it returns; its result is returned.
int run_game_on_own_thread(int (*game)(int, char**), int argc, char** argv);

// True on any thread once the game runs on its own thread (the render loop then belongs to main).
bool game_thread_active();

// The render thread's body, run on the main thread. join() waits for it to return.
void post_to_main(std::shared_ptr<std::packaged_task<void()>> task);
class MainThreadTask {
 public:
  MainThreadTask() = default;
  template <class Body> explicit MainThreadTask(Body&& body) {
    auto task = std::make_shared<std::packaged_task<void()>>(std::forward<Body>(body));
    done_ = task->get_future();
    post_to_main(std::move(task));
  }
  bool joinable() const { return done_.valid(); }
  void join() { if (done_.valid()) done_.get(); }
 private:
  std::future<void> done_;
};

}  // namespace mac_host
