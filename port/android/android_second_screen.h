// A handheld's second screen (BottomScreen.java, a Presentation of MeleeActivity): its surface and its touches, handed from
// Android's threads to the renderer, which draws the companion pages there (gx/companion.h).
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct ANativeWindow;

namespace android_host {

struct SecondSurface {
  ANativeWindow* window = nullptr;   // null: no second screen now
  int width = 0, height = 0;         // pixels
  float density = 1.0f;              // Android's display density (1 = 160 dpi)
  uint64_t generation = 0;           // changes whenever the surface is replaced, resized or taken away (0: none)
};
// Render thread, every frame: the current surface's generation. When it differs from the one the
// renderer draws on, the renderer lets go of its own (destroying what it made on it, then
// second_screen_release) and takes the new one (second_screen_acquire).
uint64_t second_screen_generation();
// The surface as it is now, with a reference of the renderer's own on the window (window null: none).
SecondSurface second_screen_acquire();
// Done with it: drops that reference. Android, waiting to take the surface away, goes on.
void second_screen_release(SecondSurface& surface);
// Whether the bottom screen is wanted at all (a second screen opened): the companion runs only then.
bool second_screen_present();

struct Touch {
  int action;    // 0 down, 1 move, 2 up, 3 cancel (every pointer)
  int pointer;   // Android's pointer id
  float x, y;    // surface pixels
};
// Render thread: the touches since the last call, oldest first.
std::vector<Touch> second_screen_touches();

// ---- a second game on the second screen (MeleeActivity.launchViewer: ViewerActivity, a process of
// its own): an arena room's match, or a replay watched there while this game goes on. Any thread.
bool viewer_launch(const std::vector<std::string>& args);   // the game's own arguments (--replay ...)
bool viewer_running();
void viewer_close();
// Leave the game and start it again with these arguments (a replay on the main screen); come_back:
// once that run ends, the game starts again as it was. The caller then asks the game to exit.
bool relaunch(const std::string& args, bool come_back);
// Whether there is a second screen to put a viewer on.
bool second_display();
// The game thread, just before the process ends (host::end_process): a relaunch asked for is handed to
// the launcher now, while the game's activity is still in front (Java's own teardown never runs:
// the process ends at once).
void before_process_end();

}  // namespace android_host
