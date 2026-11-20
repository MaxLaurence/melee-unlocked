// The disc for a launch from Finder (no --iso): a file panel, before SDL or the game starts.
// SPDX-License-Identifier: GPL-2.0-or-later
#import <AppKit/AppKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <cstring>

extern "C" bool mac_pick_iso(char* out, size_t capacity) {
  @autoreleasepool {
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    [NSApp activateIgnoringOtherApps:YES];
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.title = @"Choose your Super Smash Bros. Melee disc";
    panel.message = @"Melee Unlocked needs your own NTSC 1.02 disc image (.iso, .gcm or .rvz). It is read, never copied.";
    panel.prompt = @"Play";
    panel.canChooseFiles = YES;
    panel.canChooseDirectories = NO;
    panel.allowsMultipleSelection = NO;
    NSMutableArray<UTType*>* types = [NSMutableArray array];
    for (NSString* ext in @[@"iso", @"gcm", @"rvz", @"ciso", @"gcz"])
      if (UTType* t = [UTType typeWithFilenameExtension:ext]) [types addObject:t];
    if (types.count) panel.allowedContentTypes = types;
    if ([panel runModal] != NSModalResponseOK || !panel.URL) return false;
    const char* path = panel.URL.fileSystemRepresentation;
    if (!path || std::strlen(path) + 1 > capacity) return false;
    std::strcpy(out, path);
    return true;
  }
}
