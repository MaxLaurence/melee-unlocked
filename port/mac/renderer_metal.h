// What the Mac host needs from the Metal renderer beyond the shared interface (renderer_gl.h): the
// Metal device for the settings overlay, and the present pass the overlay draws into.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "gx_core.h"

namespace linux_renderer {
void* metal_native_device(gx::Backend& backend);   // id<MTLDevice>
struct MetalOverlay { void* command_buffer = nullptr; void* encoder = nullptr; void* pass = nullptr; };
// Valid inside Hooks::before_swap only: the drawable pass being finished.
MetalOverlay metal_overlay(gx::Backend& backend);
}  // namespace linux_renderer
