// macOS: the PC settings panel on its own, with no game behind it (--settings-window), on an SDL
// window drawn with Metal. The panel body is the same settings_frame the game runs.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "test_env.h"
#include "render_options.h"
#include "pc_settings.h"
#include "window.h"
#include "host.h"
#include "metal_device.h"
#include <SDL.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace app {

namespace {
constexpr int kBaseWidth = 620, kBaseHeight = 700;

// MELEE_TEST_SETTINGS_SHOT=<file.bmp> (tests): a few frames hidden, the last one written, then out.
void save_test_shot(const std::string& path, const std::vector<uint8_t>& rgba, int w, int h) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return;
  const uint32_t row = uint32_t(w) * 4, size = row * uint32_t(h);
  uint8_t header[54] = {'B', 'M'};
  auto put32 = [&](int at, uint32_t v) { for (int i = 0; i < 4; ++i) header[at + i] = uint8_t(v >> (8 * i)); };
  put32(2, 54 + size); put32(10, 54); put32(14, 40); put32(18, uint32_t(w)); put32(22, uint32_t(-h));   // top row first
  header[26] = 1; header[28] = 32; put32(34, size);
  std::fwrite(header, 1, sizeof header, f);
  for (size_t i = 0; i < rgba.size(); i += 4) {
    const uint8_t bgra[4] = {rgba[i + 2], rgba[i + 1], rgba[i], 255};
    std::fwrite(bgra, 1, 4, f);
  }
  std::fclose(f);
  host::log("settings: test shot %dx%d written to %s", w, h, path.c_str());
}
}  // namespace

int run_settings_window(gx::RenderOptions& options, unsigned long standby_parent, std::function<void()> reload) {
  (void)standby_parent;
  (void)reload;
  const char* shot_env = host::test_env::getenv("MELEE_TEST_SETTINGS_SHOT");
  const std::string shot = shot_env ? shot_env : "";
  gx::settings_fill_window(true);
  void* window = host::window_create(kBaseWidth, kBaseHeight, L"Melee Unlocked settings", shot.empty());
  if (!window) { host::log("settings: cannot create a window"); return 1; }
  SDL_Window* sdl = static_cast<SDL_Window*>(window);
  std::string error;
  auto device = mtl::Device::create(sdl, true, &error);
  if (!device) { host::log("settings: no Metal (%s)", error.c_str()); return 1; }
  {
    int dw = kBaseWidth, dh = kBaseHeight;
    device->drawable_size(&dw, &dh);
    if (dw > kBaseWidth + 8) gx::settings_set_window_scale(float(dw) / kBaseWidth);   // a Retina display
  }
  options.pc_settings = true;
  int frames = 0;
  {
    gx::PcSettingsUIMetal ui(window, device->native_device(), options);
    while (!host::window_closed()) {
      host::window_pump();
      host::PadState pads[4];
      host::input_poll(pads);   // the controller display and the port list follow live input here too
      int w = 0, h = 0;
      device->drawable_size(&w, &h);
      ui.begin(options);
      device->begin_frame();
      mtl::PassDesc pass;
      pass.drawable = true; pass.color_load = mtl::Load::Clear;
      pass.clear_color[0] = 0.06f; pass.clear_color[1] = 0.08f; pass.clear_color[2] = 0.12f; pass.clear_color[3] = 1.0f;
      bool done = false;
      if (device->begin_pass(pass)) {
        ui.draw(device->native_command_buffer(), device->native_encoder(), device->native_pass_descriptor());
        if (!shot.empty() && ++frames == 8) {
          std::vector<uint8_t> rgba;
          if (device->read_pixels(nullptr, 0, 0, w, h, rgba)) save_test_shot(shot, rgba, w, h);
          done = true;
        }
      }
      device->end_frame(true);
      if (done) break;
      if (!(SDL_GetWindowFlags(sdl) & SDL_WINDOW_INPUT_FOCUS)) std::this_thread::sleep_for(std::chrono::milliseconds(120));
      if (gx::settings_close_requested()) break;
    }
  }
  return 0;
}

}  // namespace app
