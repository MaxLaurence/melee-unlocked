// The companion: pages for a handheld's second screen (the AYN Thor's bottom panel), worked by touch
// while the game runs on the main screen.
//
//   Settings  the settings panel itself, moved off the game's picture (pc_settings.cpp draws it here)
//   Online    a connect code typed on a touch keyboard, the search; the match's players, ping and
//             rollbacks; the last game's Slippi-style stats
//   Training  the controller as the game read it, each fighter's action, frame and percent, frame
//             advantage, the L-cancel history; save and load state, the Training tools
//   Replays   the recordings with their players and stats; a replay playing here gets a timeline
//   Room      the warm-up room and arena rooms: who is where, the score, the room's match on this
//             screen (a second game, Android: ViewerActivity on the other display)
//   Sticks    the handheld's built-in sticks (Android, AYN): live values and their calibration
//
// It is drawn in an ImGui context of its own (sharing the panel's fonts) by settings_frame, on a
// renderer that has a second screen (the Vulkan renderer on Android). Everything here runs on the
// render thread and reads the game through what is already safe there (snapshots, atomics); what it
// changes goes through queued requests the game thread takes. Built in with MELEE_COMPANION (the
// Android build); elsewhere the functions are empty and available() is false.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <string>
#include "imgui.h"

namespace gx {
struct RenderOptions;
struct Frame;
}

namespace companion {

enum Page : int { kSettings = 0, kOnline, kTraining, kReplays, kRoom, kSticks, kPageCount };

bool available();

// Any thread: the Source Port wrote a recording (the Online page's last game, the Replays list).
void replay_saved(const std::string& path);
// Render thread, once per game frame shown (settings_set_hud_snapshot): the players and the scene.
void note_frame(const gx::Frame& frame);

// The second screen's units: ImGui units per pixel for a display of this density (Android's: 1 is
// 160 dpi), with the player's size choice.
float ui_scale(float density);

// The area the pages take (ImGui units); by default the whole display of the current context. A
// development preview (MELEE_COMPANION_PREVIEW=1) puts them beside the game in its own window.
void set_area(ImVec2 origin, ImVec2 size);
// MELEE_COMPANION_PAGE=online|training|replays|room|sticks|settings: the page to start on (tests).

// ---- one frame of the second screen, with its ImGui context current (pc_settings.cpp) ----
struct Begin {
  Page page = kOnline;
  ImVec2 page_min, page_max;   // the page's area; on kSettings the settings panel fills it
};
// The rail of pages. `panel_opened`: the controller opened the settings this frame (Back, F1, the
// chord), so the Settings page comes up.
Begin begin(bool panel_opened);
struct Result {
  bool tools_changed = false;     // practice_cpu's Training tools changed: apply and save them
  bool options_changed = false;   // a RenderOptions value changed: save
};
// The page for every Page but kSettings, in begin's area.
Result page(gx::RenderOptions& options);
// The settings panel closed itself (its Close, B): back to the page before it.
void leave_settings();

}  // namespace companion
