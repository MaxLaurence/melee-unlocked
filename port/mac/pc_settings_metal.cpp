// macOS: the PC settings panel on the Metal renderer, through Dear ImGui's Metal backend. The panel
// itself (pc_settings.cpp) is the one every platform draws; this file only renders it.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "pc_settings.h"
#include "pc_settings_shared.h"
#include "host.h"
#include "imgui.h"
#include "imgui_metal_bridge.h"
#include <memory>

namespace gx {

struct PcSettingsUIMetal::Impl {
  SettingsState state;
  bool ready = false;
};

PcSettingsUIMetal::PcSettingsUIMetal(void* window, void* metal_device, const RenderOptions& options)
    : impl_(std::make_unique<Impl>()) {
  const bool open = options.settings_open || settings_fills_window();
  impl_->state.open = open;
  impl_->state.panel_anim_target_open = open;
  impl_->state.panel_anim_initialized = true;
  impl_->state.panel_anim_frame = open ? 17.0f : 12.0f;
  impl_->state.panel_slide_x = open ? 0.0f : -720.0f;
  impl_->state.panel_slide_start_x = impl_->state.panel_slide_x;
  settings_context_create(window, options.settings_open);
  impl_->ready = mac_imgui::init(metal_device);
  if (!impl_->ready) host::log("metal: settings panel unavailable (its renderer could not start)");
}

PcSettingsUIMetal::~PcSettingsUIMetal() {
  if (impl_->ready) mac_imgui::shutdown();
  settings_context_destroy();
}

bool PcSettingsUIMetal::begin(RenderOptions& options) {
  if (!impl_->ready) return false;
  return settings_frame(impl_->state, options);
}

void PcSettingsUIMetal::draw(void* command_buffer, void* encoder, void* pass_descriptor) {
  if (!impl_->ready) return;
  mac_imgui::render(ImGui::GetDrawData(), command_buffer, encoder, pass_descriptor);
}

}  // namespace gx
