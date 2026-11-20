// The green button's own full-screen toggle (AppKit), with SDL told nothing first: Escape uses it to
// leave a full screen the player entered with the green button; MELEE_TEST_TOGGLE in window_sdl.inl
// uses it to test that path. (The OpenGL build's surface upkeep, mac_gl.mm, is not needed with Metal:
// the renderer sizes its layer to the window every frame.)
// SPDX-License-Identifier: GPL-2.0-or-later
#import <AppKit/AppKit.h>

extern "C" bool mac_window_is_fullscreen(void* nswindow) {
  return ([(__bridge NSWindow*)nswindow styleMask] & NSWindowStyleMaskFullScreen) != 0;
}
extern "C" void mac_toggle_fullscreen(void* nswindow) {
  @autoreleasepool {
    [(__bridge NSWindow*)nswindow toggleFullScreen:nil];
  }
}
