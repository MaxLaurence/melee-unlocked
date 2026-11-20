// Android renderer factory: the Vulkan GX renderer (runtime/gx/gx_vulkan.cpp, the one Windows and Linux
// offer) on the SDL window's Android surface. Android has no desktop OpenGL and no Direct3D; every
// Android device with Vulkan 1.3 (Adreno 7xx and newer, recent Mali and Xclipse) runs it.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "gx_backend.h"
#include "gx_vulkan.h"
#include "render_options.h"
#include "host.h"
#include <SDL.h>
#include <SDL_vulkan.h>
#include <SDL_syswm.h>
#include <functional>
#include <string>
#include <vector>

namespace gx {

bool d3d11_available() { return false; }

Backend* create_render_backend(void* hwnd, int client_w, int client_h, const RenderOptions& options) {
  auto* window = static_cast<SDL_Window*>(hwnd);
  VulkanWindow vw;
  vw.window = window;
  vw.drawable_size = [window](int* w, int* h) { SDL_Vulkan_GetDrawableSize(window, w, h); };
  vw.instance_extensions = [window] {
    unsigned n = 0;
    SDL_Vulkan_GetInstanceExtensions(window, &n, nullptr);
    std::vector<const char*> names(n);
    SDL_Vulkan_GetInstanceExtensions(window, &n, names.data());
    return names;
  };
  vw.create_surface = [window](void* instance, void* surface) {
    // In the background the window has no Android surface (SDL drops it): nothing to make one from yet.
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(window, &info) || !info.info.android.window) return false;
    return SDL_Vulkan_CreateSurface(window, static_cast<VkInstance>(instance), static_cast<VkSurfaceKHR*>(surface)) == SDL_TRUE;
  };
  RenderOptions vulkan_options = options;
  vulkan_options.api = RenderApi::Vulkan;
  vulkan_options.dlss_mode = 0;
  vulkan_options.frame_generation_mode = 0;
  if (Backend* backend = create_vulkan_backend(vw, client_w, client_h, vulkan_options)) return backend;
  host::log("vulkan: the renderer did not start on this device");
  host::log_flush();
  SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Melee Unlocked",
                           "Melee Unlocked needs Vulkan 1.3 graphics, which this device's driver did not start.", window);
  host::end_process(1);
  return nullptr;
}

}  // namespace gx

// The launcher's standalone settings window (--settings-window) does not exist on Android: the panel
// opens over the game instead.
namespace app {
int run_settings_window(gx::RenderOptions&, unsigned long, std::function<void()>) { return 1; }
}
