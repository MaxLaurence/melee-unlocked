// macOS renderer factory: the Metal GX renderer (renderer_metal.cpp) with the settings panel drawn
// over each presented frame, as the Windows and Linux renderers do. The Mac has one graphics API;
// the Direct3D, Vulkan and NVIDIA (DLSS, Reflex, frame generation) options do not exist here.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "gx_backend.h"
#include "gx_vulkan.h"
#include "pc_settings.h"
#include "pc_settings_shared.h"
#include "host.h"
#include "window.h"
#include "../linux/renderer_gl.h"
#include "renderer_metal.h"
#include "embed_mac.h"
#include "exi_slippi.h"
#include "gx_shader.h"
#include <SDL.h>
#include <algorithm>
#include <memory>
#include <string>

namespace gx {

namespace {
struct MacPresentation {
  RenderOptions options;
  std::unique_ptr<PcSettingsUIMetal> settings;
  Backend* renderer = nullptr;
};
std::unique_ptr<MacPresentation> g_presentation;

// Dolphin's "Auto (Window Size)": the integer EFB scale that covers the window, as the Windows
// renderers pick it; a fixed choice from the settings is used as is. Supersampling renders at 2x, 3x
// or 4x the chosen scale and the present pass averages it down.
int mac_scale(const RenderOptions& options, int client_h) {
  const int ssaa = std::clamp(options.ssaa, 1, 4);
  if (options.efb_scale > 0) return std::clamp(options.efb_scale * ssaa, 1, 6);
  return std::clamp((client_h + EFB_HEIGHT - 1) / EFB_HEIGHT * ssaa, 1, 6);
}
}  // namespace

bool d3d11_available() { return false; }

// No Vulkan on the Mac build (the renderer is Metal): the settings panel never offers it.
bool vulkan_available() { return false; }
int vulkan_failed_starts() { return 0; }
void vulkan_set_shader_cache(const std::string&) {}
void vulkan_forget_failed_starts() {}
Backend* create_vulkan_backend(const VulkanWindow&, int, int, const RenderOptions&) { return nullptr; }

Backend* create_render_backend(void* hwnd, int client_w, int client_h, const RenderOptions& options) {
  auto* window = static_cast<SDL_Window*>(hwnd);
  g_presentation = std::make_unique<MacPresentation>();
  g_presentation->options = options;
  RenderOptions& opts = g_presentation->options;
  // Whatever graphics API a settings file from Windows or Linux names, the Mac draws with Metal; NVIDIA's
  // upscaling and frame generation have no Metal counterpart.
  opts.dlss_mode = 0;
  opts.frame_generation_mode = 0;
  const int scale = mac_scale(opts, client_h);
  std::unique_ptr<Backend> renderer;
  try {
    renderer = linux_renderer::create(window, opts.shader_cache.empty() ? "shadercache" : opts.shader_cache, scale, opts.vsync);
  } catch (const std::exception& e) {
    host::log("metal: the renderer did not start: %s", e.what());
    host::log_flush();
    if (!host::options.no_gc_adapter)   // set for hidden and headless runs: a test never waits on a dialog
      SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Melee Unlocked",
          ("Melee Unlocked could not start Metal graphics on this Mac.\n\n" + std::string(e.what())).c_str(), window);
    host::end_process(1);
  }
  host::log("metal: internal resolution EFB x%d (window %dx%d)", scale, client_w, client_h);
  g_presentation->renderer = renderer.get();
  if (opts.pc_settings)
    g_presentation->settings = std::make_unique<PcSettingsUIMetal>(hwnd, linux_renderer::metal_native_device(*renderer), opts);
  linux_renderer::Hooks hooks;
  hooks.options = &opts;
  hooks.frame_begin = [](const Frame& frame) {
    auto& p = *g_presentation;
    p.options.fullscreen = host::window_is_fullscreen();
    // As the Windows renderers: the Slippi widescreen code, Fountain of Dreams reflections and True
    // widescreen follow the settings every frame (they are game changes, not drawing).
    static bool widescreen_sent = false, fod_sent = false;
    if (p.options.widescreen != widescreen_sent) { widescreen_sent = p.options.widescreen; slippi::request_widescreen(widescreen_sent); }
    if (p.options.fod_reflections != fod_sent) { fod_sent = p.options.fod_reflections; slippi::request_fod_reflections(fod_sent); }
    {
      int cw = 0, ch = 0;
      host::window_client_size(&cw, &ch);
      set_true_widescreen(p.options.true_widescreen && !p.options.widescreen, true_widescreen_aspect(p.options, cw, ch));
    }
    if (!p.settings) return;
    int w = 0, h = 0;
    host::window_client_size(&w, &h);
    settings_set_game_aspect(presented_aspect(p.options, w, h, false));
    settings_set_hud_snapshot(frame);
    // A changed V-Sync reaches the layer through the renderer's next frame (it reads the options).
    if (p.settings->begin(p.options)) host::window_set_fullscreen(p.options.fullscreen);
  };
  hooks.before_swap = [](int, int) {
    auto& p = *g_presentation;
    if (!p.settings || !p.renderer) return;
    const auto target = linux_renderer::metal_overlay(*p.renderer);
    p.settings->draw(target.command_buffer, target.encoder, target.pass);
  };
  hooks.after_swap = [window](uint64_t frame) {
    mac_embed::frame_presented(window);
    host::window_presented(frame);
  };
  linux_renderer::set_hooks(*renderer, std::move(hooks));
  return renderer.release();
}

}  // namespace gx
