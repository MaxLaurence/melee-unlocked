// The companion's touch input (companion.h): the second screen's touches, fed to its ImGui context.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstddef>

namespace companion {

struct Touch {
  int action;    // 0 down, 1 move, 2 up, 3 cancel
  int pointer;   // the platform's pointer id
  float x, y;    // pixels
};
// With the companion's ImGui context current, before its NewFrame: the touches since the last frame
// and the UI scale (pixels per ImGui unit). A tap clicks, a drag on a slider moves it, and a drag
// anywhere else scrolls what is under it.
void feed_touches(const Touch* touches, size_t count, float scale);

}  // namespace companion
