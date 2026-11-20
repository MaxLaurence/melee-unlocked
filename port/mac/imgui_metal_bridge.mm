// See imgui_metal_bridge.h. ARC, no Win32 compatibility headers.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "imgui_metal_bridge.h"
#import <Metal/Metal.h>
#include "imgui.h"
#include "backends/imgui_impl_metal.h"

namespace mac_imgui {
bool init(void* device) { return device && ImGui_ImplMetal_Init((__bridge id<MTLDevice>)device); }
void shutdown() { ImGui_ImplMetal_Shutdown(); }
void render(ImDrawData* data, void* command_buffer, void* encoder, void* pass_descriptor) {
  if (!data || !command_buffer || !encoder || !pass_descriptor) return;
  @autoreleasepool {
    ImGui_ImplMetal_NewFrame((__bridge MTLRenderPassDescriptor*)pass_descriptor);
    ImGui_ImplMetal_RenderDrawData(data, (__bridge id<MTLCommandBuffer>)command_buffer,
                                   (__bridge id<MTLRenderCommandEncoder>)encoder);
  }
}
}  // namespace mac_imgui
