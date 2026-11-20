// Dear ImGui's Metal renderer behind plain pointers, for code built with the Win32 compatibility
// headers (which Objective-C cannot see). Implemented in imgui_metal_bridge.mm.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
struct ImDrawData;
namespace mac_imgui {
bool init(void* mtl_device);
void shutdown();
// Draws `data` into the given pass: id<MTLCommandBuffer>, id<MTLRenderCommandEncoder> and the
// MTLRenderPassDescriptor it was made from (ImGui builds its pipeline for that pass's formats).
void render(ImDrawData* data, void* command_buffer, void* encoder, void* pass_descriptor);
}  // namespace mac_imgui
