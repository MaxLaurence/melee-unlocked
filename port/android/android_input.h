// Android's controllers beyond what SDL reads (port/android/android_input.cpp):
//  - the GameCube adapter, which an app reaches only through Android's USB host API: MeleeActivity asks
//    the player's permission, opens it and hands its file descriptor to libusb (runtime/host/gc_adapter.cpp);
//  - a handheld's built-in sticks as slippi-android reads them on the AYN Thor: unclipped values from
//    AYN's mapping service, with that app's stick calibration (deadzone, response curve, outer scale).
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>

namespace android_host {
// The adapter's USB file descriptor once the player allowed it (the Java side opens and claims it), or
// -1 when none is plugged in or permission is still being asked. Any thread.
int gc_adapter_open();
// Gives the adapter back (the Java side closes its connection, and with it the descriptor).
void gc_adapter_close();

// The built-in controls' sticks for an SDL pad: when (vendor, product) is the handheld's own pad, its
// four stick axes (XInput's: -32768..32767, Y up) are replaced by the calibrated ones, from the raw
// reader when it is live, else from the values SDL read. Returns whether the pad is the built-in one.
bool builtin_sticks(uint16_t vendor, uint16_t product, int16_t& lx, int16_t& ly, int16_t& rx, int16_t& ry);

// ---- the companion's Sticks page (gx/companion.h): the calibration, seen and changed live ----
struct StickCalibration {
  float center_x = 0, center_y = 0;                                    // the stick at rest, -1..1
  float scale_x_pos = 1, scale_x_neg = 1, scale_y_pos = 1, scale_y_neg = 1;   // outward, per direction
  float deadzone = 0.02f, curve = 1.5f, cap = 1.0f;
};
// Whether the handheld's own pad is known (an AYN handheld), and whether the raw reader is live.
bool builtin_pad_present();
// The newest raw values (left x, y, right x, y: -1..1, right and down positive); false when the raw
// reader has not delivered in the last 100 ms.
bool builtin_raw(float out[4]);
void builtin_calibration(StickCalibration& main, StickCalibration& c);
void set_builtin_calibration(const StickCalibration& main, const StickCalibration& c);   // from the next read on
bool save_builtin_calibration();      // builtin-sticks.ini in the player's folder
void default_builtin_calibration(StickCalibration& main, StickCalibration& c);
// One stick through a calibration (raw: the unclipped reader's values, which take the outer scale).
void calibrate_stick(const StickCalibration& cal, float x, float y, bool raw, float& out_x, float& out_y);
}  // namespace android_host
