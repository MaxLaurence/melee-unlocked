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
}  // namespace android_host
