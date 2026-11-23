// Metal backend for the shared GX command decoder (macOS). The Mac counterpart of the OpenGL renderer
// (port/linux/renderer_gl.cpp), implementing the same linux_renderer interface (renderer_gl.h) so the
// rest of the host is unchanged.
//
// The generated GX shaders stay the one TEV/lighting implementation: their HLSL is translated to MSL
// in the program (shader_msl.cpp) and cached on disk. Metal's coordinate conventions are Direct3D's
// (clip z 0..1, y up; framebuffer and textures with row 0 at the top), so the viewport, scissor,
// depth and copy arithmetic is the Direct3D 11 backend's, not the OpenGL one's.
//
// Shaders and pipelines are built on worker threads. A draw whose pipeline is not ready yet is
// skipped for those few frames (MELEE_SYNC_SHADERS=1 waits instead); every pipeline a run builds is
// recorded and the next start builds them all before the game runs.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "test_env.h"
#include "../linux/renderer_gl.h"
#include "../runtime/hle/yougame_parity.h"
#include "../runtime/host/window.h"
#include "host.h"
#include "gx_shader.h"
#include "gx_texture.h"
#include "texture_pack.h"
#include "video_background.h"
#include "video_background_learning.h"
#include "crt_shader.h"
#include "gx_retro.h"
#include "metal_device.h"
#include "renderer_metal.h"
#include "shader_msl.h"
#include "platform_paths.h"
#include <SDL.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <unistd.h>

namespace gx {
unsigned gx_capture_request();
void gx_capture_request_set(unsigned frames);
}

namespace linux_renderer {
namespace {

using Clock = std::chrono::steady_clock;
double ms_since(Clock::time_point start) { return std::chrono::duration<double, std::milli>(Clock::now() - start).count(); }

void metal_log(const char* fmt, ...) {
  char line[2048];
  va_list args; va_start(args, fmt);
  std::vsnprintf(line, sizeof line, fmt, args); va_end(args);
  std::fprintf(stderr, "%s\n", line);
  host::log("%s", line);
}
std::string read_text(const std::filesystem::path& path) {
  std::ifstream f(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(f), {});
}
// Cache writes never stop the game: a full disk or read-only folder only loses the speedup.
void write_cache(const std::filesystem::path& path, const std::string& data) {
  static std::atomic<unsigned> serial{0};
  const auto partial = path.string() + "." + std::to_string(getpid()) + "." + std::to_string(serial++);
  { std::ofstream f(partial, std::ios::binary); f.write(data.data(), std::streamsize(data.size())); if (!f) return; }
  std::error_code ec; std::filesystem::rename(partial, path, ec);
  if (ec) std::filesystem::remove(partial, ec);
}
double env_ms(const char* name, double fallback) {
  const char* v = std::getenv(name);
  if (!v || !*v) return fallback;
  char* end = nullptr; const double value = std::strtod(v, &end);
  return end != v && !*end && std::isfinite(value) && value >= 0 ? value : fallback;
}

// The renderer's own passes: EFB copies, clears, video layers, border art and the present pass.
const char* kInternalShaders = R"(
#include <metal_stdlib>
using namespace metal;
struct Quad { float4 position [[position]]; float2 p; };
// One triangle over the target; p runs 0..1 from the target's top left.
vertex Quad quad_vs(uint id [[vertex_id]]) {
  const float2 p = float2((id << 1) & 2, id & 2);
  Quad o; o.position = float4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0); o.p = p; return o;
}
struct CopyParams { float4 rect; int kind; };
// rect: size and origin of the source region, as fractions of the source (v counted from the top).
fragment float4 copy_fs(Quad in [[stage_in]], constant CopyParams& c [[buffer(0)]],
                        texture2d<float> src [[texture(0)]], sampler s [[sampler(0)]]) {
  float4 color = src.sample(s, c.rect.zw + in.p * c.rect.xy);
  if (c.kind == 1) color.a = 1.0;
  return color;
}
// A depth copy as the game reads one: the 24-bit GX depth in the colour channels.
fragment float4 copy_depth_fs(Quad in [[stage_in]], constant CopyParams& c [[buffer(0)]],
                              depth2d<float> src [[texture(0)]], sampler s [[sampler(0)]]) {
  const float d = src.sample(s, c.rect.zw + in.p * c.rect.xy);
  const uint z = min(uint((1.0 - d) * 16777216.0), 0xFFFFFFu);
  return float4(float3((z >> 16) & 255u, (z >> 8) & 255u, z & 255u) / 255.0, 1.0);
}
// A solid colour (the first launch's progress bar on the window, which has no depth buffer).
fragment float4 fill_fs(Quad in [[stage_in]], constant float4& c [[buffer(0)]]) { return c; }
struct ClearParams { float4 color; float depth; };
struct ClearOut { float4 color [[color(0)]]; float depth [[depth(any)]]; };
fragment ClearOut clear_fs(Quad in [[stage_in]], constant ClearParams& c [[buffer(0)]]) {
  ClearOut o; o.color = c.color; o.depth = c.depth; return o;
}

// The picture on the window: the EFB's frame region averaged over each output pixel when it is
// larger (supersampling), sharpened, with brightness, contrast and vibrance, or drawn through the CRT
// tube model. The OpenGL renderer's present pass, with t and p counted from the top.
struct PictureParams { float4 rect, sharp, box, col, crt, crt2; int crt_on; };
float2 picture_uv(constant PictureParams& u, float2 t) { return u.rect.zw + t * u.rect.xy; }
float4 downsample(constant PictureParams& u, texture2d<float> src, sampler s, float2 uv) {
  const int nx = int(u.box.x), ny = int(u.box.y);
  if (nx <= 1 && ny <= 1) return src.sample(s, uv);
  const float2 foot = u.sharp.xy * float2(nx, ny);
  float4 acc = float4(0.0);
  for (int y = 0; y < ny; ++y)
    for (int x = 0; x < nx; ++x) acc += src.sample(s, uv + (float2(x, y) + 0.5) / float2(nx, ny) * foot - 0.5 * foot);
  return acc / float(nx * ny);
}
float3 crt_linear(float3 c) { return pow(clamp(c, 0.0, 1.0), float3(2.4)); }
float3 crt_at(constant PictureParams& u, texture2d<float> src, sampler s, float2 t) {
  const float2 half_texel = 0.5 * u.sharp.xy / u.rect.xy;
  return crt_linear(src.sample(s, picture_uv(u, clamp(t, half_texel, 1.0 - half_texel))).rgb);
}
float3 crt_scanline(constant PictureParams& u, texture2d<float> src, sampler s, float2 t, float line_y, float2 cell) {
  const float ox[4] = {-0.9, -0.3, 0.3, 0.9};
  const float wx[4] = {0.262, 0.862, 0.862, 0.262};
  float3 acc = float3(0.0);
  for (int k = 0; k < 4; ++k) {
    const float x = t.x + ox[k] * cell.x;
    acc += wx[k] * (crt_at(u, src, s, float2(x, line_y - 0.25 * cell.y)) + crt_at(u, src, s, float2(x, line_y + 0.25 * cell.y)));
  }
  return acc / (2.0 * 2.248);
}
float3 crt_beam(float3 c, float d, float depth, float pixel) {
  const float3 level = sqrt(clamp(c, 0.0, 1.0));
  float3 sigma = mix(float3(mix(0.62, 0.24, depth)), float3(mix(0.62, 0.43, depth)), level);
  sigma = sqrt(sigma * sigma + pixel * pixel / 12.0);
  return c * exp(-0.5 * d * d / (sigma * sigma)) / (sigma * 2.5066);
}
float3 crt_mask(float2 pos, float depth, float per_line) {
  if (depth <= 0.0) return float3(1.0);
  float3 m;
  if (per_line >= 2.8) {
    const int x = int(pos.x) % 3;
    m = x == 0 ? float3(1, 0, 0) : (x == 1 ? float3(0, 1, 0) : float3(0, 0, 1));
    return mix(float3(1.0), m, depth) / (1.0 - depth * 2.0 / 3.0);
  }
  m = (int(pos.x) & 1) != 0 ? float3(0, 1, 0) : float3(1, 0, 1);
  return mix(float3(1.0), m, depth) / (1.0 - depth * 0.5);
}
float3 crt_pixel(constant PictureParams& u, texture2d<float> src, sampler s, float2 pos, float2 t) {
  const float2 one_pixel = fwidth(t);
  float face = 1.0;
  if (u.crt.z > 0.0) {
    float2 q = t * 2.0 - 1.0;
    q *= 1.0 + u.crt.z * float2(q.y * q.y, q.x * q.x);
    q /= 1.0 + u.crt.z * 0.5;
    const float2 px = 4.0 * one_pixel;
    const float2 e = smoothstep(0.0, 1.0, (1.0 - abs(q)) / max(px, float2(1e-5)));
    const float2 corner = max(abs(q) - (1.0 - 0.045), 0.0) / 0.045;
    face = e.x * e.y * clamp((1.0 - length(corner)) / max(max(px.x, px.y) / 0.045, 1e-5), 0.0, 1.0);
    face *= 1.0 - 0.35 * u.crt.z * dot(q, q) * 0.5;
    t = q * 0.5 + 0.5;
  }
  if (face <= 0.0) return float3(0.0);
  const float lines = max(u.crt.w, 1.0), per_line = max(u.crt2.y, 0.01);
  const float2 cell = 1.0 / float2(max(u.crt2.x, 1.0), lines);
  const float depth = u.crt.x * clamp(per_line - 1.0, 0.0, 1.0);
  const float y = t.y * lines - 0.5 + u.crt2.w;
  const float upper = floor(y), d = y - upper;
  const float3 a = crt_scanline(u, src, s, t, (upper + 0.5) * cell.y, cell);
  const float3 b = crt_scanline(u, src, s, t, (upper + 1.5) * cell.y, cell);
  const float pixel = 1.0 / per_line;
  float3 c = crt_beam(a, d, depth, pixel) + crt_beam(b, 1.0 - d, depth, pixel);
  if (u.crt2.z > 0.0) {
    const float2 h = 1.6 * cell;
    const float3 halo = 0.25 * (crt_at(u, src, s, t + float2(-h.x, -h.y)) + crt_at(u, src, s, t + float2(h.x, -h.y)) +
                                crt_at(u, src, s, t + float2(-h.x, h.y)) + crt_at(u, src, s, t + h));
    c = mix(c, max(c, halo), u.crt2.z);
  }
  c *= crt_mask(pos, u.crt.y, per_line);
  return pow(clamp(c * face, 0.0, 1.0), float3(1.0 / 2.2));
}
fragment float4 picture_fs(Quad in [[stage_in]], constant PictureParams& u [[buffer(0)]],
                           texture2d<float> src [[texture(0)]], sampler s [[sampler(0)]]) {
  const float2 uv = u.rect.zw + in.p * u.rect.xy;
  float3 rgb;
  if (u.crt_on != 0) {
    rgb = crt_pixel(u, src, s, in.position.xy, in.p);
  } else {
    rgb = downsample(u, src, s, uv).rgb;
    if (u.sharp.z > 0.0) {
      // Contrast-adaptive sharpening over neighbours one output pixel away.
      const float2 st = u.sharp.xy * max(u.box.xy, float2(1.0));
      const float3 n = src.sample(s, uv + float2(0, st.y)).rgb, so = src.sample(s, uv - float2(0, st.y)).rgb;
      const float3 w = src.sample(s, uv - float2(st.x, 0)).rgb, e = src.sample(s, uv + float2(st.x, 0)).rgb;
      const float3 mn = min(min(min(n, so), min(w, e)), rgb), mx = max(max(max(n, so), max(w, e)), rgb);
      const float3 amp = sqrt(clamp(min(mn, 1.0 - mx) / max(mx, float3(1e-4)), 0.0, 1.0));
      const float3 wgt = amp * (-1.0 / mix(8.0, 5.0, clamp(u.sharp.z, 0.0, 1.0)));
      rgb = clamp((rgb + (n + so + w + e) * wgt) / (1.0 + 4.0 * wgt), 0.0, 1.0);
    }
  }
  rgb = (rgb * u.col.x - 0.5) * u.col.y + 0.5;
  const float luma = dot(rgb, float3(0.2126, 0.7152, 0.0722));
  return float4(clamp(mix(float3(luma), rgb, u.col.z), 0.0, 1.0), 1.0);
}
)";

struct CopyParams { float rect[4]; int32_t kind; int32_t pad[3]; };
struct ClearParams { float color[4]; float depth; float pad[3]; };
struct PictureParams { float rect[4], sharp[4], box[4], col[4], crt[4], crt2[4]; int32_t crt_on; int32_t pad[3]; };

// The draw state a GX pipeline is generated from (as the OpenGL renderer's recipes). Every pipeline a
// run builds is recorded and the next start builds them all before the game runs.
struct ProgramRecipe {
  uint32_t components = 0;
  gx::BPMemory bp{};
  uint32_t xf[0x58]{};
};
constexpr uint64_t RECIPE_MAGIC = 0x3150524C544D5847ull;   // "GXMTLRP1"
constexpr size_t RECIPE_LIMIT = 16384;

struct ProgramKey {
  uint64_t vertex, pixel;
  bool operator==(const ProgramKey& o) const { return vertex == o.vertex && pixel == o.pixel; }
};
struct ProgramKeyHash { size_t operator()(const ProgramKey& k) const { return size_t(k.vertex ^ (k.pixel * 31)); } };

// A GX program's two functions and stream layout, built once by whichever worker gets to it first.
struct Program {
  std::string vertex_hlsl, pixel_hlsl;
  uint64_t vertex_hash = 0, pixel_hash = 0;
  std::once_flag built;
  bool ok = false;
  std::string error;
  mtl::Function vertex = nullptr, pixel = nullptr;
  std::vector<mtl::VertexAttribute> attributes;
};

uint32_t blend_key(const mtl::BlendDesc& b) {
  return (b.enabled ? 1u : 0u) | uint32_t(b.src) << 1 | uint32_t(b.dst) << 5 | uint32_t(b.op) << 9 |
         (b.write_rgb ? 1u << 10 : 0u) | (b.write_alpha ? 1u << 11 : 0u);
}
uint64_t pipeline_key(const ProgramKey& p, uint32_t blend, uint32_t samples) {
  return p.vertex * 0x9E3779B97F4A7C15ull ^ (p.pixel + 0x632BE59BD9B4E019ull) * 31 ^ (uint64_t(blend) << 8 | samples) * 0xC2B2AE3D27D4EB4Full;
}

// The GX draw's blend state, as the Direct3D 11 and OpenGL backends set it.
mtl::BlendDesc gx_blend(const gx::DrawCall& dc) {
  using F = mtl::Factor;
  static const F src[] = {F::Zero, F::One, F::DstColor, F::OneMinusDstColor, F::SrcAlpha, F::OneMinusSrcAlpha, F::DstAlpha, F::OneMinusDstAlpha};
  static const F dst[] = {F::Zero, F::One, F::SrcColor, F::OneMinusSrcColor, F::SrcAlpha, F::OneMinusSrcAlpha, F::DstAlpha, F::OneMinusDstAlpha};
  const uint32_t blend = dc.bp.blendmode();
  mtl::BlendDesc b;
  b.enabled = (blend & 1) != 0;
  F sf = src[gx::bits(blend, 8, 3)], df = dst[gx::bits(blend, 5, 3)];
  if ((dc.bp.zcontrol() & 7) != 1) {   // the EFB keeps no alpha (anything but RGBA6_Z24)
    if (sf == F::DstAlpha) sf = F::One; if (sf == F::OneMinusDstAlpha) sf = F::Zero;
    if (df == F::DstAlpha) df = F::One; if (df == F::OneMinusDstAlpha) df = F::Zero;
  }
  if (!b.enabled) { sf = F::One; df = F::Zero; }   // one key for every disabled state
  b.src = sf; b.dst = df;
  b.src_alpha = F::One; b.dst_alpha = F::Zero;
  b.op = b.enabled && (blend & (1u << 11)) ? mtl::BlendOp::ReverseSubtract : mtl::BlendOp::Add;
  b.op_alpha = mtl::BlendOp::Add;
  b.write_rgb = (blend & 8) != 0;
  b.write_alpha = (blend & 16) != 0;
  return b;
}

// The stream layout of gx::Vertex, by the HLSL input names of the generated vertex shaders.
bool vertex_attribute(const std::string& name, uint32_t location, mtl::VertexAttribute& out) {
  using V = mtl::VertexFormat;
  struct Entry { const char* name; V format; uint32_t offset; };
  static const Entry table[] = {
    {"rawpos", V::Float3, 0}, {"rawnorm0", V::Float3, 12}, {"color0", V::UChar4Normalized, 24}, {"color1", V::UChar4Normalized, 28},
    {"rawtex0", V::Float2, 32}, {"rawtex1", V::Float2, 40}, {"rawtex2", V::Float2, 48}, {"rawtex3", V::Float2, 56},
    {"rawtex4", V::Float2, 64}, {"rawtex5", V::Float2, 72}, {"rawtex6", V::Float2, 80}, {"rawtex7", V::Float2, 88},
    {"blend_indices", V::UChar4, 96}, {"blend_indices2", V::UChar4, 100}, {"blend_index7", V::UChar, 104}};
  for (const auto& e : table)
    if (name == e.name) { out = {location, e.format, e.offset}; return true; }
  return false;
}
static_assert(sizeof(gx::Vertex) == 108, "vertex layout");

struct Texture {
  mtl::Texture tex = nullptr;
  uint32_t width = 0, height = 0, levels = 1;
  uint64_t used = 0, bytes = 0;
  uint64_t replacement_bytes = 0;   // charged to the texture pack budget; given back when the cache drops it
};

class Renderer final : public gx::Backend {
 public:
  Renderer(SDL_Window* window, const std::filesystem::path& cache, int scale, bool vsync)
      : window_(window), cache_(cache / "gx-metal-v1"), scale_(std::clamp(scale, 1, 6)),
        width_(gx::EFB_WIDTH * scale_), height_(gx::EFB_HEIGHT * scale_), vsync_(vsync) {
    std::error_code ec;
    std::filesystem::create_directories(cache_, ec);
    // MELEE_SHADER_SEED (the bundle's Resources/shader-seed, port/mac/full_launch.h): pipeline recipes
    // and translated shaders recorded by port/mac/make_shader_seed.py, so a first launch builds them
    // before its first frame instead of skipping each new effect while it builds. Empty: no seed.
    if (const char* seed = std::getenv("MELEE_SHADER_SEED"); seed && *seed) {
      const auto folder = std::filesystem::path(seed) / cache_.filename();
      if (std::filesystem::is_directory(folder, ec)) seed_ = folder;
    }
    std::string error;
    dev_ = mtl::Device::create(window_, vsync, &error);
    if (!dev_) throw std::runtime_error("Metal: " + error);
    metal_log("metal: %s; EFB %dx%d (x%d)", dev_->name().c_str(), width_, height_, scale_);
    efb_color_ = dev_->new_texture(width_, height_, 1, mtl::Format::RGBA8, true, 1, "EFB colour");
    efb_depth_ = dev_->new_texture(width_, height_, 1, mtl::Format::Depth32F, true, 1, "EFB depth");
    last_color_ = dev_->new_texture(width_, height_, 1, mtl::Format::RGBA8, true, 1, "last frame");
    if (!efb_color_ || !efb_depth_ || !last_color_) throw std::runtime_error("Metal: cannot allocate the EFB");
    for (const char* name : {"quad_vs", "copy_fs", "copy_depth_fs", "clear_fs", "picture_fs", "fill_fs"}) {
      mtl::Function f = dev_->new_function(kInternalShaders, name, &error);
      if (!f) throw std::runtime_error(std::string("Metal internal shader ") + name + ": " + error);
      internal_[name] = f;
    }
    mtl::SamplerDesc linear; linear.min = linear.mag = mtl::Filter::Linear;
    mtl::SamplerDesc nearest; nearest.min = nearest.mag = mtl::Filter::Nearest;
    linear_sampler_ = dev_->new_sampler(linear);
    nearest_sampler_ = dev_->new_sampler(nearest);
    const uint32_t white = 0xFFFFFFFFu;
    white_ = dev_->new_texture(1, 1, 1, mtl::Format::RGBA8, false, 1, "white");
    dev_->upload(white_, 0, 1, 1, &white, 4);
    const unsigned cores = std::max(2u, std::thread::hardware_concurrency());
    const unsigned workers = std::clamp(cores / 3, 2u, 6u);
    for (unsigned i = 0; i < workers; ++i) workers_.emplace_back([this] { work(); });
    metal_log("metal: shaders built on %u worker threads%s", workers, sync_shaders_ ? " (MELEE_SYNC_SHADERS: draws wait for them)" : "");
  }
  ~Renderer() override {
    {
      std::lock_guard<std::mutex> lock(jobs_mutex_);
      stopping_ = true; jobs_.clear();
    }
    jobs_cv_.notify_all();
    for (auto& t : workers_) t.join();
    save_recipes();
    if (skipped_draws_total_) metal_log("metal: %llu draws were skipped while their shaders were being built",
                                        (unsigned long long)skipped_draws_total_);
    if (dev_) {
      drain_results();
      for (auto& p : pipelines_) dev_->release(p.second);
      for (auto& p : programs_) { dev_->release(p.second->vertex); dev_->release(p.second->pixel); }
      for (auto& p : internal_pipelines_) dev_->release(p.second);
      for (auto& f : internal_) dev_->release(f.second);
      for (auto& t : textures_) release(t.second);
      for (auto& t : copies_) dev_->release(t.second.tex);
      for (auto& s : samplers_) dev_->release(s.second);
      for (auto& v : video_) dev_->release(v.tex);
      dev_->release(linear_sampler_); dev_->release(nearest_sampler_);
      dev_->release(white_); dev_->release(efb_color_); dev_->release(efb_depth_); dev_->release(last_color_);
      dev_->release(msaa_color_); dev_->release(msaa_depth_);
      dev_.reset();
    }
  }

  void presentation_stats(uint32_t* frames, uint32_t* pipes, uint32_t* textures) const override {
    if (frames) *frames = presented_;
    if (pipes) *pipes = uint32_t(pipelines_.size());
    if (textures) *textures = uint32_t(textures_.size() + copies_.size());
  }
  std::string profile_line() const override { return "metal: " + (dev_ ? dev_->name() : std::string("none")); }
  void set_skip_present(bool skip) override { skip_ = skip; }
  void set_hooks(Hooks hooks) { hooks_ = std::move(hooks); }
  const gx::RenderOptions* presentation_options() const override { return hooks_.options; }
  void submit_frame(const gx::Frame& frame) override { submit_frame(frame, nullptr); }
  void submit_frame(const gx::Frame& frame, const gx::DrawMatrices* overrides) override {
    source_retrace_ = frame.source_retrace;
    if (hooks_.frame_begin) hooks_.frame_begin(frame);
    widenable_scene_ = gx::frame_has_widenable_scene(frame);   // as the D3D backends: 16:9 reaches the screen
    const int lightweight = hooks_.options ? hooks_.options->lightweight : 0;
    gx::set_lightweight_level(lightweight);
    if (hooks_.options) gx::publish_look(*hooks_.options);
    effects_filter_ = gx::effects_filter(frame, gx::lightweight_effects_level(hooks_.options ? hooks_.options->effects_level : 0, lightweight));
    lightweight_filter_ = gx::lightweight_filter(frame, lightweight);
    if (hooks_.options) {
      // Texture packs switched on or off, or a skin picked on the character select: every cached
      // texture was built with or without its replacement (as the Windows renderers).
      gx::video_bg::set_enabled(hooks_.options->video_backgrounds);
      gx::video_bg::begin_frame(frame.scene_major, frame.scene_minor);
      if (gx::texpack::configure(hooks_.options->custom_textures, hooks_.options->dump_textures) || gx::texpack::take_cosmetics_changed()) {
        for (auto& t : textures_) release(t.second);
        textures_.clear();
        replacement_bytes_ = 0;
      }
      if (hooks_.options->anisotropy != anisotropy_applied_) {
        anisotropy_applied_ = hooks_.options->anisotropy;
        for (auto& s : samplers_) dev_->release(s.second);
        samplers_.clear();
      }
      if (hooks_.options->vsync != vsync_) { vsync_ = hooks_.options->vsync; dev_->set_vsync(vsync_); }
    }
    drain_results();
    // The recorded pipelines, once the anti-aliasing they are built for is known (the first frame),
    // before that frame begins: the wait shows its own progress on the window.
    if (!prewarmed_) {
      apply_msaa(hooks_.options ? hooks_.options->msaa : 1);
      prewarmed_ = true;
      prewarm();
    }
    dev_->begin_frame();
    in_frame_ = true;
    apply_msaa(hooks_.options ? hooks_.options->msaa : 1);
    ++sequence_;
    // The frame's vertices once, then each override draw's own vertex stream after them.
    const size_t original_count = frame.vertices.size();
    vertex_offsets_.assign(frame.draws.size(), 0);
    size_t total_count = original_count;
    if (overrides) for (size_t i = 0; i < frame.draws.size(); ++i)
      if (overrides[i].vertices) { vertex_offsets_[i] = total_count; total_count += frame.draws[i].vertex_count; }
    vertices_ = dev_->alloc(std::max<size_t>(total_count, 1) * sizeof(gx::Vertex), 16);
    if (original_count) std::memcpy(vertices_.cpu, frame.vertices.data(), original_count * sizeof(gx::Vertex));
    if (overrides) for (size_t i = 0; i < frame.draws.size(); ++i)
      if (overrides[i].vertices)
        std::memcpy(static_cast<uint8_t*>(vertices_.cpu) + vertex_offsets_[i] * sizeof(gx::Vertex), overrides[i].vertices,
                    frame.draws[i].vertex_count * sizeof(gx::Vertex));
    gx::set_main_projection(gx::frame_scene_draw(frame));
    gx::set_css_scene(gx::video_bg::learning::scene_slot(frame.scene_major, frame.scene_minor) == 0);
    // Menu video backgrounds: the character select's video goes in after Melee's untextured animated
    // backdrop, the stage select's right after the learned backdrop draw (as on Windows).
    const gx::EfbCopy* screen = nullptr;
    for (const auto& copy : frame.copies) if (copy.to_xfb) screen = &copy;
    int fullscreen_slot = -1;
    const std::shared_ptr<const gx::video_bg::Frame> fullscreen = gx::video_bg::fullscreen_frame(&fullscreen_slot);
    const bool css_fullscreen = fullscreen && fullscreen_slot == 0 && screen;
    unsigned leading_untextured = 0;
    bool background_drawn = false;
    present_pending_ = false;
    for (const auto& command : frame.commands) {
      if (command.kind == gx::FrameCommand::Draw) {
        const auto& dc = frame.draws.at(command.index);
        if (css_fullscreen && !background_drawn) {
          bool textured = false;
          for (const auto& texture : dc.textures) textured |= texture.used;
          if (!textured) ++leading_untextured;
          else if (gx::video_bg::learning::should_insert_fullscreen_layer(fullscreen_slot, leading_untextured, textured)) {
            draw_video_layer(fullscreen, fullscreen_slot, *screen);
            background_drawn = true;
          }
        }
        draw_video_slot_ = -1;
        draw(frame, dc, overrides ? overrides + command.index : nullptr, vertex_offsets_[command.index]);
        if (fullscreen && screen && fullscreen_slot == 1 && draw_video_slot_ == 1 && !background_drawn) {
          draw_video_layer(fullscreen, fullscreen_slot, *screen);
          background_drawn = true;
        }
      } else {
        const auto& c = frame.copies.at(command.index);
        if (c.to_xfb) { if (!skip_) { capture_sequence_ = frame.sequence; present(c, frame); } }
        else copy(c);
        if (c.clear) clear(c);
      }
    }
    dev_->end_frame(present_pending_);
    in_frame_ = false;
    efb_pass_ = false;
    if (present_pending_) after_present(frame);
    trim();
    report_skips(false);
  }

  // --capture-frame / --capture-sim-frame / --capture-every / --capture-burst, and F2 (the next N
  // frames into capture/), as the Windows renderers write them.
  void captures(uint64_t sequence, int w, int h) {
    const gx::RenderOptions* o = hooks_.options;
    const uint32_t n = presented_;
    try {
      if (o && !o->capture_path.empty()) {
        if (o->capture_sim_frame && !capture_frame_ && sequence >= o->capture_sim_frame) capture_frame_ = n;
        const uint32_t at = capture_frame_ ? capture_frame_ : o->capture_frame;
        const bool burst = o->capture_burst && at && n >= at && n < at + o->capture_burst;
        if (n == at && !burst) capture_window(o->capture_path, w, h);
        else if (burst || (o->capture_every && n % o->capture_every == 0)) {
          const std::string& base = o->capture_path;
          char suffix[48];
          std::snprintf(suffix, sizeof suffix, "_%05u.ppm", n);
          capture_window(base.substr(0, base.size() > 4 && base.compare(base.size() - 4, 4, ".ppm") == 0 ? base.size() - 4 : base.size()) + suffix, w, h);
        }
      }
      if (unsigned want = gx::gx_capture_request()) {
        gx::gx_capture_request_set(want - 1);
        std::filesystem::create_directories("capture");
        char name[64];
        std::snprintf(name, sizeof name, "capture/blink_%05u.ppm", n);
        capture_window(name, w, h);
        if (want == 1) std::fprintf(stderr, "capture: wrote the requested frames into capture/\n");
      }
    } catch (const std::exception& e) {
      std::fprintf(stderr, "capture: %s\n", e.what());
    }
  }
  static void write_ppm(const std::filesystem::path& path, const std::vector<uint8_t>& rgba, int w, int h) {
    std::ofstream f(path, std::ios::binary); f << "P6\n" << w << " " << h << "\n255\n";
    std::vector<uint8_t> row(size_t(w) * 3);
    for (int y = 0; y < h; ++y) {
      for (int x = 0; x < w; ++x) for (int k = 0; k < 3; ++k) row[size_t(x) * 3 + k] = rgba[(size_t(y) * w + x) * 4 + k];
      f.write(reinterpret_cast<const char*>(row.data()), std::streamsize(row.size()));
    }
    if (!f) throw std::runtime_error("Cannot write frame capture: " + path.string());
  }
  // The window as it is about to be shown (picture, border art, the settings and practice menus).
  void capture_window(const std::filesystem::path& path, int w, int h) {
    std::vector<uint8_t> rgba;
    const bool was_in_pass = dev_->in_pass();
    if (!dev_->read_pixels(nullptr, 0, 0, w, h, rgba)) return;
    write_ppm(path, rgba, w, h);
    if (was_in_pass) resume_drawable_pass();
  }
  // The game's last presented frame at EFB resolution (--capture at the end of a run).
  void capture(const std::filesystem::path& path) {
    std::vector<uint8_t> rgba;
    with_frame([&] { dev_->read_pixels(have_last_ ? last_color_ : efb_color_, 0, 0, width_, height_, rgba); });
    if (rgba.empty()) throw std::runtime_error("Metal: frame capture read nothing");
    write_ppm(path, rgba, width_, height_);
  }
  bool check_smoke_pixels() {
    std::array<uint8_t, 4> background{}, red{}, blue{}, texture{};
    with_frame([&] {
      auto pixel = [this](int x, int y) {
        std::vector<uint8_t> px;
        dev_->read_pixels(efb_color_, x * scale_, y * scale_, 1, 1, px);
        std::array<uint8_t, 4> c{};
        if (px.size() >= 4) std::memcpy(c.data(), px.data(), 4);
        return c;
      };
      background = pixel(0, 0); red = pixel(150, 400); blue = pixel(320, 200); texture = pixel(500, 275);
    });
    auto near = [](uint8_t v, int target) { return std::abs(int(v) - target) <= 3; };
    return background[0] == 0 && background[1] == 0 && background[2] == 0 &&
           red[0] > 180 && red[1] < 50 && red[2] < 50 &&
           blue[2] > 150 && blue[0] < 65 && blue[1] < 65 &&
           near(texture[0], 200) && near(texture[1], 200) && near(texture[2], 200);
  }
  void compare_culling(const gx::Frame& frame, const std::filesystem::path& prefix) {
    const bool saved = clockwise_front_;
    clockwise_front_ = true; submit_frame(frame); capture(prefix.string() + ".cw.ppm");
    clockwise_front_ = false; submit_frame(frame); capture(prefix.string() + ".ccw.ppm");
    clockwise_front_ = saved; submit_frame(frame);
  }
  void* native_device() const { return dev_->native_device(); }
  MetalOverlay overlay() const {
    return MetalOverlay{dev_->native_command_buffer(), dev_->native_encoder(), dev_->native_pass_descriptor()};
  }
  // Waits until every queued shader and pipeline is built (the self-test's first frame).
  void finish_builds(double timeout_ms) {
    const auto start = Clock::now();
    for (;;) {
      drain_results();
      if (pending_.empty() || ms_since(start) > timeout_ms) return;
      std::unique_lock<std::mutex> lock(results_mutex_);
      results_cv_.wait_for(lock, std::chrono::milliseconds(5), [&] { return !results_.empty(); });
    }
  }

 private:
  // ---------------------------------------------------------------- frames and passes
  template <class F> void with_frame(F&& body) {
    const bool outside = !in_frame_;
    if (outside) { dev_->begin_frame(); in_frame_ = true; }
    body();
    if (outside) { dev_->end_frame(false); in_frame_ = false; }
  }
  mtl::Texture draw_color() const { return msaa_color_ ? msaa_color_ : efb_color_; }
  mtl::Texture draw_depth() const { return msaa_depth_ ? msaa_depth_ : efb_depth_; }
  // Draws, clears and video layers go to the EFB (its multisampled targets with MSAA, which resolve
  // into the EFB textures whenever the pass ends: before every copy, present and capture).
  void ensure_efb_pass() {
    if (efb_pass_ && dev_->in_pass()) return;
    mtl::PassDesc d;
    d.color = draw_color(); d.depth = draw_depth();
    if (msaa_color_) { d.resolve_color = efb_color_; d.resolve_depth = efb_depth_; }
    d.color_load = d.depth_load = efb_fresh_ ? mtl::Load::Clear : mtl::Load::Load;
    d.clear_depth = 0;
    efb_fresh_ = false;
    efb_pass_ = dev_->begin_pass(d);
    if (efb_pass_) dev_->set_vertex_buffer(vertices_, mtl::kVertexBufferIndex);
  }
  void end_efb_pass() {
    if (efb_pass_) dev_->end_pass();
    efb_pass_ = false;
  }
  void apply_msaa(int wanted) {
    const uint32_t n = dev_->supported_samples(uint32_t(std::clamp(wanted, 1, 8)));
    if (n == msaa_samples_) return;
    end_efb_pass();
    dev_->release(msaa_color_); dev_->release(msaa_depth_);
    msaa_color_ = msaa_depth_ = nullptr;
    msaa_samples_ = 1;
    if (n > 1) {
      msaa_color_ = dev_->new_texture(width_, height_, 1, mtl::Format::RGBA8, true, n, "EFB colour MSAA");
      msaa_depth_ = dev_->new_texture(width_, height_, 1, mtl::Format::Depth32F, true, n, "EFB depth MSAA");
      if (!msaa_color_ || !msaa_depth_) {
        dev_->release(msaa_color_); dev_->release(msaa_depth_); msaa_color_ = msaa_depth_ = nullptr;
        metal_log("metal: %ux MSAA unavailable", n);
      } else {
        msaa_samples_ = n;
      }
    }
    efb_fresh_ = true;
    metal_log("metal: anti-aliasing %ux MSAA", msaa_samples_);
  }

  // ---------------------------------------------------------------- pipelines
  mtl::Pipeline internal_pipeline(const char* fragment, mtl::Format color, mtl::Format depth, uint32_t samples,
                                  bool depth_write = false) {
    const std::string key = std::string(fragment) + ":" + std::to_string(int(color)) + ":" + std::to_string(int(depth)) +
                            ":" + std::to_string(samples);
    if (auto i = internal_pipelines_.find(key); i != internal_pipelines_.end()) return i->second;
    mtl::PipelineDesc d;
    d.vertex = internal_.at("quad_vs"); d.fragment = internal_.at(fragment);
    d.color = color; d.depth = depth; d.samples = samples; d.label = fragment;
    (void)depth_write;
    std::string error;
    mtl::Pipeline p = dev_->new_pipeline(d, &error);
    if (!p) throw std::runtime_error(std::string("Metal internal pipeline ") + fragment + ": " + error);
    internal_pipelines_.emplace(key, p);
    return p;
  }
  // The pipeline for a draw, or nullptr while it is still being built.
  mtl::Pipeline pipeline(const gx::DrawCall& dc, const mtl::BlendDesc& blend) {
    const ProgramKey pk{gx::make_vs_uid(dc).hash(), gx::make_ps_uid(dc).hash()};
    const uint64_t key = pipeline_key(pk, blend_key(blend), msaa_samples_);
    if (auto i = pipelines_.find(key); i != pipelines_.end()) return i->second;
    if (failed_.count(key)) return nullptr;
    if (!pending_.count(key)) {
      remember_recipe(dc);
      request(pk, dc, blend, msaa_samples_, key);
    }
    if (sync_shaders_ || prewarming_) {
      const auto start = Clock::now();
      while (pending_.count(key) && ms_since(start) < 30000) {
        std::unique_lock<std::mutex> lock(results_mutex_);
        results_cv_.wait_for(lock, std::chrono::milliseconds(2), [&] { return !results_.empty(); });
        lock.unlock();
        drain_results();
      }
    } else {
      drain_results();
    }
    if (auto i = pipelines_.find(key); i != pipelines_.end()) return i->second;
    return nullptr;
  }
  void request(const ProgramKey& pk, const gx::DrawCall& dc, const mtl::BlendDesc& blend, uint32_t samples, uint64_t key) {
    auto& program = programs_[pk];
    if (!program) {
      program = std::make_shared<Program>();
      program->vertex_hlsl = gx::generate_vertex_shader(gx::make_vs_uid(dc));
      program->pixel_hlsl = gx::generate_pixel_shader(gx::make_ps_uid(dc));
      program->vertex_hash = gx::hash_bytes(program->vertex_hlsl.data(), program->vertex_hlsl.size());
      program->pixel_hash = gx::hash_bytes(program->pixel_hlsl.data(), program->pixel_hlsl.size());
    }
    pending_.insert(key);
    {
      std::lock_guard<std::mutex> lock(jobs_mutex_);
      jobs_.push_back(Job{key, program, blend, samples, prewarming_});
    }
    jobs_cv_.notify_one();
  }
  struct Job { uint64_t key; std::shared_ptr<Program> program; mtl::BlendDesc blend; uint32_t samples; bool prewarm; };
  struct Result { uint64_t key; mtl::Pipeline pipeline; std::string error; bool prewarm; double ms; };
  void work() {
    for (;;) {
      Job job;
      {
        std::unique_lock<std::mutex> lock(jobs_mutex_);
        jobs_cv_.wait(lock, [&] { return stopping_ || !jobs_.empty(); });
        if (stopping_) return;
        job = std::move(jobs_.front()); jobs_.pop_front();
      }
      const auto start = Clock::now();
      Result r{job.key, nullptr, {}, job.prewarm, 0};
      try {
        build_program(*job.program);
        if (!job.program->ok) throw std::runtime_error(job.program->error);
        mtl::PipelineDesc d;
        d.vertex = job.program->vertex; d.fragment = job.program->pixel;
        d.attributes = job.program->attributes; d.vertex_stride = sizeof(gx::Vertex);
        d.color = mtl::Format::RGBA8; d.depth = mtl::Format::Depth32F; d.samples = job.samples;
        d.blend = job.blend;
        std::string error;
        r.pipeline = dev_->new_pipeline(d, &error);
        if (!r.pipeline) r.error = error;
      } catch (const std::exception& e) {
        r.error = e.what();
      }
      r.ms = ms_since(start);
      {
        std::lock_guard<std::mutex> lock(results_mutex_);
        results_.push_back(std::move(r));
      }
      results_cv_.notify_all();
    }
  }
  // Both stages: the cached MSL, or a translation of the HLSL written to the cache; then Metal's own
  // compile. Any worker; the first to need the program builds it.
  void build_program(Program& p) {
    std::call_once(p.built, [&] {
      try {
        mac_shaders::Msl stage[2];
        for (int i = 0; i < 2; ++i) {
          const bool vertex = i == 0;
          std::ostringstream name; name << (vertex ? "vs-" : "ps-") << std::hex << (vertex ? p.vertex_hash : p.pixel_hash) << ".msl";
          const auto path = cache_ / name.str();
          if (!mac_shaders::parse(read_text(path), stage[i]) &&
              (seed_.empty() || !mac_shaders::parse(read_text(seed_ / name.str()), stage[i]))) {
            std::string error;
            if (!mac_shaders::translate(vertex ? p.vertex_hlsl : p.pixel_hlsl, vertex, stage[i], &error))
              throw std::runtime_error((vertex ? "vertex shader: " : "pixel shader: ") + error);
            write_cache(path, mac_shaders::serialize(stage[i]));
            ++translated_;
          }
        }
        for (const auto& input : stage[0].inputs) {
          mtl::VertexAttribute a;
          if (!vertex_attribute(input.name, input.location, a)) throw std::runtime_error("unknown vertex input " + input.name);
          p.attributes.push_back(a);
        }
        // MELEE_METAL_SOURCE_SALT (tests, an identifier): a declaration that makes every shader new to macOS's own compiled
        // shader cache, to time a first launch on a Mac that never ran the game.
        static const std::string salt = [] { const char* v = std::getenv("MELEE_METAL_SOURCE_SALT"); return v && *v ? "\nconstant uint melee_salt_" + std::string(v) + " = 1u;\n" : std::string(); }();
        if (!salt.empty()) { stage[0].source += salt; stage[1].source += salt; }
        std::string error;
        p.vertex = dev_->new_function(stage[0].source, stage[0].entry, &error);
        if (!p.vertex) throw std::runtime_error("Metal vertex shader: " + error);
        p.pixel = dev_->new_function(stage[1].source, stage[1].entry, &error);
        if (!p.pixel) throw std::runtime_error("Metal pixel shader: " + error);
        p.ok = true;
      } catch (const std::exception& e) {
        p.error = e.what();
        p.ok = false;
      }
    });
  }
  void drain_results() {
    std::deque<Result> done;
    { std::lock_guard<std::mutex> lock(results_mutex_); done.swap(results_); }
    for (auto& r : done) {
      pending_.erase(r.key);
      if (r.pipeline) {
        pipelines_.emplace(r.key, r.pipeline);
        if (!r.prewarm) log_new_pipeline(r.key, r.ms);
      } else {
        failed_.insert(r.key);
        static unsigned logged = 0;
        if (logged++ < 20) metal_log("metal: a GX shader failed to build (its draws are skipped): %s", r.error.c_str());
      }
    }
  }
  void log_new_pipeline(uint64_t key, double ms) {
    static unsigned logged = 0;
    if (logged < 400) {
      ++logged;
      metal_log("metal: new pipeline at frame %u (sim %u) in %.1f ms (%016llx)", presented_, source_retrace_, ms, (unsigned long long)key);
    } else if (logged == 400) {
      ++logged;
      metal_log("metal: further new pipelines not logged");
    }
  }
  void report_skips(bool final) {
    const double seconds = ms_since(skip_report_) / 1000;
    if (!final && seconds < 1) return;
    if (skipped_draws_) std::fprintf(stderr, "metal: skipped %llu draws awaiting shaders in %.3f s\n",
                                     (unsigned long long)skipped_draws_, seconds);
    skipped_draws_total_ += skipped_draws_;
    skipped_draws_ = 0; skip_report_ = Clock::now();
  }
  // ---------------------------------------------------------------- recipes
  static bool parse_recipes(const std::string& data, std::vector<ProgramRecipe>& out) {
    uint64_t header[3]{};
    if (data.size() < sizeof header) return false;
    std::memcpy(header, data.data(), sizeof header);
    const size_t count = size_t(header[1]);
    if (header[0] != RECIPE_MAGIC || count > RECIPE_LIMIT || data.size() != sizeof header + count * sizeof(ProgramRecipe) ||
        gx::hash_bytes(data.data() + sizeof header, count * sizeof(ProgramRecipe)) != header[2]) return false;
    out.resize(count);
    std::memcpy(out.data(), data.data() + sizeof header, count * sizeof(ProgramRecipe));
    return true;
  }
  void remember_recipe(const gx::DrawCall& dc) {
    ProgramRecipe r; r.components = dc.components; r.bp = dc.bp;
    std::memcpy(r.xf, dc.xf_regs, sizeof r.xf);
    if (recipes_.size() < RECIPE_LIMIT && recipe_hashes_.insert(gx::hash_bytes(&r, sizeof r)).second) {
      recipes_.push_back(r);
      if (!prewarming_ && recipes_.size() >= saved_recipes_ + 32) save_recipes();
    }
  }
  void save_recipes() {
    if (recipes_.size() == saved_recipes_) return;
    saved_recipes_ = recipes_.size();
    const size_t bytes = recipes_.size() * sizeof(ProgramRecipe);
    const uint64_t header[3] = {RECIPE_MAGIC, recipes_.size(), gx::hash_bytes(recipes_.data(), bytes)};
    std::string data(sizeof header + bytes, '\0');
    std::memcpy(data.data(), header, sizeof header);
    std::memcpy(data.data() + sizeof header, recipes_.data(), bytes);
    write_cache(cache_ / "recipes.bin", data);
  }
  // Every recorded pipeline is built again at start, on all the workers, before the game runs. A
  // first start of a new build translates its shaders (a few seconds); later starts find the MSL in
  // the cache and Metal's own compiled copies.
  void prewarm() {
    std::vector<ProgramRecipe> list, seeded;
    parse_recipes(read_text(cache_ / "recipes.bin"), list);
    const size_t own = list.size();
    if (!seed_.empty() && parse_recipes(read_text(seed_ / "recipes.bin"), seeded))
      list.insert(list.end(), seeded.begin(), seeded.end());
    if (list.empty()) return;
    const auto start = Clock::now();
    prewarming_ = true;
    size_t queued = 0;
    std::vector<ProgramRecipe> seed_used;   // one seed recipe per pipeline (MELEE_SHADER_SEED_COMPACT)
    for (size_t i = 0; i < list.size(); ++i) {
      const ProgramRecipe& r = list[i];
      // The player's own recipes stay theirs (and are saved again); the seed's are only built.
      const bool mine = i < own;
      if (mine) {
        if (recipes_.size() >= RECIPE_LIMIT || !recipe_hashes_.insert(gx::hash_bytes(&r, sizeof r)).second) continue;
        recipes_.push_back(r);
      }
      gx::DrawCall dc{}; dc.components = r.components; dc.bp = r.bp;
      std::memcpy(dc.xf_regs, r.xf, sizeof r.xf);
      const ProgramKey pk{gx::make_vs_uid(dc).hash(), gx::make_ps_uid(dc).hash()};
      const mtl::BlendDesc blend = gx_blend(dc);
      const uint64_t key = pipeline_key(pk, blend_key(blend), msaa_samples_);
      if (pending_.count(key) || pipelines_.count(key)) continue;
      if (!mine) seed_used.push_back(r);
      request(pk, dc, blend, msaa_samples_, key);
      ++queued;
    }
    saved_recipes_ = recipes_.size();
    // make_shader_seed.py: the merged runs hold the same pipeline many times over (a recipe keeps every
    // register, most of which no shader reads); this writes one recipe per pipeline.
    if (const char* compact = std::getenv("MELEE_SHADER_SEED_COMPACT"); compact && *compact) {
      const size_t bytes = seed_used.size() * sizeof(ProgramRecipe);
      const uint64_t header[3] = {RECIPE_MAGIC, seed_used.size(), gx::hash_bytes(seed_used.data(), bytes)};
      std::string data(sizeof header + bytes, '\0');
      std::memcpy(data.data(), header, sizeof header);
      std::memcpy(data.data() + sizeof header, seed_used.data(), bytes);
      write_cache(compact, data);
      metal_log("metal: shader seed compacted to %zu pipelines", seed_used.size());
    }
    const double limit = env_ms("MELEE_PREWARM_BOOT_MS", 60000);
    size_t last_shown = SIZE_MAX;
    auto last_progress = start;
    while (!pending_.empty() && ms_since(start) < limit) {
      {
        std::unique_lock<std::mutex> lock(results_mutex_);
        results_cv_.wait_for(lock, std::chrono::milliseconds(20), [&] { return !results_.empty(); });
      }
      drain_results();
      const size_t done = queued - pending_.size();
      if (ms_since(start) > 300 && done != last_shown && ms_since(last_progress) > 33) {
        last_shown = done;
        last_progress = Clock::now();
        progress_screen(done, queued);
        host::loading_show(L"Preparing shaders", done, queued);
      }
    }
    prewarming_ = false;
    if (last_shown != SIZE_MAX) host::loading_close();
    metal_log("metal: prewarmed %zu of %zu pipelines from %zu recipes (%zu of them the shipped seed's) in %.1f ms (%zu translated now)%s",
              pipelines_.size(), queued, list.size(), list.size() - own, ms_since(start), translated_.load(),
              pending_.empty() ? "" : "; the rest continue in the background");
  }
  // The wait on the window: a bar across the middle, filled as the pipelines are built.
  void progress_screen(size_t done, size_t total) {
    if (!total) return;
    int w = 0, h = 0;
    if (!dev_->drawable_size(&w, &h) || w <= 0 || h <= 0) return;
    dev_->begin_frame();
    mtl::PassDesc d;
    d.drawable = true;
    d.color_load = mtl::Load::Clear;
    d.clear_color[0] = 0.035f; d.clear_color[1] = 0.035f; d.clear_color[2] = 0.045f; d.clear_color[3] = 1.0f;
    if (!dev_->begin_pass(d)) { dev_->end_frame(false); return; }
    const mtl::Pipeline fill = internal_pipeline("fill_fs", mtl::Format::BGRA8, mtl::Format::None, 1);
    dev_->set_pipeline(fill);
    dev_->set_depth_state(dev_->depth_state(false, mtl::Compare::Always, false));
    dev_->set_cull(mtl::Cull::None, true);
    dev_->set_fill_lines(false);
    dev_->set_viewport(0, 0, float(w), float(h), 0, 1);
    const int bw = std::max(w / 2, 8), bh = std::max(h / 90, 4), bx = (w - bw) / 2, by = h / 2 - bh / 2;
    const float track[4] = {0.16f, 0.16f, 0.19f, 1.0f}, bar[4] = {0.95f, 0.76f, 0.18f, 1.0f};
    dev_->set_scissor(bx, by, bw, bh);
    dev_->set_fragment_bytes(track, sizeof track, 0);
    dev_->draw(mtl::Primitive::Triangles, 0, 3);
    const int filled = int(double(bw) * double(std::min(done, total)) / double(total));
    if (filled > 0) {
      dev_->set_scissor(bx, by, filled, bh);
      dev_->set_fragment_bytes(bar, sizeof bar, 0);
      dev_->draw(mtl::Primitive::Triangles, 0, 3);
    }
    dev_->end_pass();
    dev_->end_frame(true);
  }

  // ---------------------------------------------------------------- textures
  void release(Texture& t) {
    replacement_bytes_ -= std::min(replacement_bytes_, t.replacement_bytes);
    dev_->release(t.tex); t.tex = nullptr;
  }
  mtl::Texture texture(const gx::TextureRef& t) {
    if (auto c = copies_.find(t.addr & 0x3FFFFFFFu); c != copies_.end()) { c->second.used = sequence_; return c->second.tex; }
    if (!t.data || !t.width || !t.height) return nullptr;
    if (gx::video_bg::wants_texture_names()) {
      int slot = -1;
      const auto video = gx::video_bg::lookup(gx::texpack::base_name(t, *t.data), t.width, t.height, &slot);
      if (video && slot >= 0 && slot < 2 && !video->bgra.empty())
        if (mtl::Texture tex = video_texture(video, slot)) { draw_video_slot_ = slot; return tex; }
    }
    const uint64_t desc[] = {t.data->hash, t.width, t.height, t.format, t.tlut_format, t.mip_levels};
    const uint64_t key = gx::look_key(gx::hash_bytes(desc, sizeof desc), look_surface_);
    if (auto i = textures_.find(key); i != textures_.end()) { i->second.used = sequence_; return i->second.tex; }
    if (auto small = gx::look_replacement(t, look_surface_)) {   // the look's treated copy, one level (gx_retro.h)
      Texture entry; entry.width = small->width; entry.height = small->height; entry.used = sequence_; entry.levels = 1;
      entry.tex = dev_->new_texture(small->width, small->height, 1, mtl::Format::RGBA8, false);
      dev_->upload(entry.tex, 0, small->width, small->height, small->pixels.data(), size_t(small->width) * 4);
      entry.bytes = small->pixels.size();
      textures_.emplace(key, entry);
      return entry.tex;
    }
    Texture entry; entry.width = t.width; entry.height = t.height; entry.used = sequence_;
    entry.levels = gx::texture_mip_count(t.width, t.height, t.mip_levels);
    // Custom texture pack or a skin's companion textures: Dolphin's name for this texture, then its
    // PNG if a source has one (once per unique texture, never per draw), as on Windows.
    std::string pack_name;
    if (gx::texpack::enabled() || gx::texpack::dumping() || gx::texpack::cosmetics_enabled()) {
      pack_name = gx::texpack::base_name(t, *t.data);
      if (gx::texpack::has(pack_name)) {
        constexpr uint64_t budget = 512ull << 20;
        std::shared_ptr<const gx::texpack::Replacement> r =
            gx::texpack::load(pack_name, replacement_bytes_ < budget ? budget - replacement_bytes_ : 0);
        gx::texpack::note_lookup(r != nullptr);
        if (r) {
          entry.width = r->width; entry.height = r->height; entry.levels = std::clamp(r->levels, 1u, 16u);
          entry.tex = dev_->new_texture(entry.width, entry.height, entry.levels, mtl::Format::RGBA8, false);
          for (uint32_t level = 0; level < entry.levels; ++level) {
            dev_->upload(entry.tex, level, r->level_width[level], r->level_height[level], r->pixels.data() + r->level_offset[level],
                         size_t(r->level_width[level]) * 4);
            entry.bytes += uint64_t(r->level_width[level]) * r->level_height[level] * 4;
          }
          replacement_bytes_ += entry.bytes;
          entry.replacement_bytes = entry.bytes;
          textures_.emplace(key, entry);
          return entry.tex;
        }
      }
    }
    entry.tex = dev_->new_texture(t.width, t.height, entry.levels, mtl::Format::RGBA8, false);
    const uint8_t* data = t.data->image.data(); size_t remaining = t.data->image.size();
    uint32_t w = t.width, h = t.height;
    for (uint32_t level = 0; level < entry.levels; ++level) {
      const uint32_t bytes = gx::texture_level_bytes(w, h, t.format);
      if (bytes > remaining) { dev_->release(entry.tex); throw std::runtime_error("GX texture mip exceeded snapshot bytes"); }
      gx::decode_texture(data, w, h, t.format, t.data->palette.data(), t.tlut_format, decoded_);
      if (gx::texpack::dumping() && !pack_name.empty()) gx::texpack::dump_level(pack_name, level, decoded_.data(), w, h);
      dev_->upload(entry.tex, level, w, h, decoded_.data(), size_t(w) * 4);
      entry.bytes += uint64_t(w) * h * 4; data += bytes; remaining -= bytes;
      w = std::max(1u, w / 2); h = std::max(1u, h / 2);
    }
    textures_.emplace(key, entry);
    return entry.tex;
  }
  mtl::Sampler sampler(uint32_t mode0, uint32_t mode1) {
    const uint64_t key = (uint64_t(mode0) << 32) | mode1;
    if (auto i = samplers_.find(key); i != samplers_.end()) return i->second;
    static const mtl::Wrap wraps[] = {mtl::Wrap::Clamp, mtl::Wrap::Repeat, mtl::Wrap::Mirror, mtl::Wrap::Repeat};
    mtl::SamplerDesc d;
    d.s = wraps[gx::bits(mode0, 0, 2)];
    d.t = wraps[gx::bits(mode0, 2, 2)];
    const uint32_t minf = gx::bits(mode0, 5, 3), mip = minf & 3; const bool linear = (minf & 4) != 0;
    d.min = linear ? mtl::Filter::Linear : mtl::Filter::Nearest;
    d.mip = !mip ? mtl::MipFilter::None : mip == 2 ? mtl::MipFilter::Linear : mtl::MipFilter::Nearest;
    d.mag = gx::bits(mode0, 4, 1) ? mtl::Filter::Linear : mtl::Filter::Nearest;
    d.min_lod = gx::bits(mode1, 0, 8) / 16.0f;
    d.max_lod = mip ? gx::bits(mode1, 8, 8) / 16.0f : 0.0f;
    // Anisotropic filtering (Video settings), on linear filters as the Windows renderers.
    if (anisotropy_applied_ > 1 && linear && gx::bits(mode0, 4, 1)) d.anisotropy = std::clamp(anisotropy_applied_, 1, 16);
    mtl::Sampler s = dev_->new_sampler(d);
    samplers_.emplace(key, s);
    return s;
  }

  // ---------------------------------------------------------------- draws
  void draw(const gx::Frame& frame, const gx::DrawCall& dc, const gx::DrawMatrices* matrices, size_t vertex_override) {
    if (gx::skip_for_effects(effects_filter_, dc)) return;
    if (gx::skip_for_lightweight(lightweight_filter_, frame, dc)) return;   // "Lightweight" 2+: scenery behind the play plane
    // Viewport and scissor (the Direct3D backends' arithmetic).
    const float* vp = reinterpret_cast<const float*>(&dc.xf_regs[0x1A]);
    const float s = float(scale_);
    float x = (vp[3] - vp[0] - 342.0f) * s, y = (vp[4] + vp[1] - 342.0f) * s, w = 2.0f * vp[0] * s, h = -2.0f * vp[1] * s;
    if (w < 0) { x += w; w = -w; }
    if (h < 0) { y += h; h = -h; }
    float znear = std::clamp(1.0f - vp[5] / 16777216.0f, 0.0f, 1.0f), zfar = std::clamp(1.0f - (vp[5] - vp[2]) / 16777216.0f, 0.0f, 1.0f);
    if (zfar < znear) std::swap(znear, zfar);
    const auto tl = dc.bp.reg[gx::BP_SCISSORTL], br = dc.bp.reg[gx::BP_SCISSORBR], so = dc.bp.reg[gx::BP_SCISSOROFFSET];
    const int ox = int(gx::bits(so, 0, 10)) * 2 - 342, oy = int(gx::bits(so, 10, 10)) * 2 - 342;
    const int l = std::clamp(int(gx::bits(tl, 12, 12)) - ox - 342, 0, gx::EFB_WIDTH), t = std::clamp(int(gx::bits(tl, 0, 12)) - oy - 342, 0, gx::EFB_HEIGHT);
    const int r = std::clamp(int(gx::bits(br, 12, 12)) - ox - 341, 0, gx::EFB_WIDTH), b = std::clamp(int(gx::bits(br, 0, 12)) - oy - 341, 0, gx::EFB_HEIGHT);
    if (r <= l || b <= t) return;
    const mtl::BlendDesc blend = gx_blend(dc);
    mtl::Pipeline p = pipeline(dc, blend);
    if (!p) { ++skipped_draws_; return; }
    ensure_efb_pass();
    if (!efb_pass_) return;
    dev_->set_pipeline(p);
    dev_->set_viewport(x, y, std::max(w, 1.0f), std::max(h, 1.0f), znear, zfar);
    dev_->set_scissor(l * scale_, t * scale_, (r - l) * scale_, (b - t) * scale_);
    // Reversed depth (GX near = 1), as the Direct3D backends.
    static const mtl::Compare compare[] = {mtl::Compare::Never, mtl::Compare::Greater, mtl::Compare::Equal, mtl::Compare::GreaterEqual,
                                           mtl::Compare::Less, mtl::Compare::NotEqual, mtl::Compare::LessEqual, mtl::Compare::Always};
    const uint32_t zmode = dc.bp.zmode();
    dev_->set_depth_state(dev_->depth_state((zmode & 1) != 0, compare[gx::bits(zmode, 1, 3)], (zmode & 16) != 0));
    const bool wire = gx::look_wire(lightweight_filter_, dc);   // "Lightweight" 5 or a Wire look
    static const mtl::Cull culls[] = {mtl::Cull::None, mtl::Cull::Back, mtl::Cull::Front, mtl::Cull::Back};
    dev_->set_cull(wire ? mtl::Cull::None : culls[dc.bp.cullmode() & 3], clockwise_front_);
    dev_->set_fill_lines(wire);
    // Constants: only the part of the vertex block this draw's shader reads is written.
    gx::fill_vs_constants(dc, vc_, scale_, matrices);
    gx::PSConstants pc{};
    gx::fill_ps_constants(dc, pc, scale_);
    const mtl::Span vs = dev_->alloc(sizeof(gx::VSConstants), 256);
    std::memcpy(vs.cpu, &vc_, gx::vs_constants_bytes(dc, false));
    dev_->set_vertex_buffer(vs, mac_shaders::kConstantsIndex);
    dev_->set_fragment_bytes(&pc, sizeof pc, mac_shaders::kConstantsIndex);
    dev_->set_vertex_buffer(vertices_, mtl::kVertexBufferIndex);
    const bool flat = gx::lightweight_flat(lightweight_filter_, dc);   // "Lightweight" 3+: one white texel instead
    look_surface_ = gx::look_surface(lightweight_filter_, dc);
    const bool unfiltered = gx::look_unfiltered(lightweight_filter_, dc);
    for (int unit = 0; unit < 8; ++unit) {
      const auto& ref = dc.textures[unit];
      if (!ref.used) { dev_->set_fragment_texture(nullptr, unit); continue; }
      dev_->set_fragment_texture(flat ? white_ : texture(ref), unit);
      dev_->set_fragment_sampler(sampler(unfiltered ? gx::unfiltered_mode0(ref.mode0) : ref.mode0, ref.mode1), unit);
    }
    // texture() may have ended the pass with a video upload; it never does today, but be sure.
    if (!dev_->in_pass()) return;
    index_scratch_.clear();
    gx::DrawTopology topology = gx::DrawTopology::Unsupported;
    if (dc.segment_count) {
      for (uint32_t i = 0; i < dc.segment_count; ++i) {
        const auto& seg = frame.segments.at(dc.first_segment + i);
        const uint32_t base = vertex_override ? uint32_t(vertex_override + seg.first_vertex - dc.first_vertex) : seg.first_vertex;
        topology = gx::append_segment_indices(index_scratch_, seg.primitive, seg.vertex_count, base);
      }
    } else {
      topology = gx::append_segment_indices(index_scratch_, dc.primitive, dc.vertex_count,
                                            vertex_override ? uint32_t(vertex_override) : dc.first_vertex);
    }
    if (index_scratch_.empty()) return;
    const mtl::Span indices = dev_->alloc(index_scratch_.size() * sizeof(uint32_t), 4);
    std::memcpy(indices.cpu, index_scratch_.data(), index_scratch_.size() * sizeof(uint32_t));
    dev_->draw_indexed(topology == gx::DrawTopology::Lines ? mtl::Primitive::Lines : mtl::Primitive::Triangles,
                       uint32_t(index_scratch_.size()), indices);
  }
  // A full-screen pass of one of the internal fragment shaders over the current pass's viewport.
  void quad(mtl::Pipeline p, mtl::Texture source, mtl::Sampler s, const void* params, size_t bytes) {
    dev_->set_pipeline(p);
    dev_->set_depth_state(dev_->depth_state(false, mtl::Compare::Always, false));
    dev_->set_cull(mtl::Cull::None, true);
    dev_->set_fill_lines(false);
    dev_->set_fragment_bytes(params, bytes, 0);
    dev_->set_fragment_texture(source, 0);
    dev_->set_fragment_sampler(s, 0);
    dev_->draw(mtl::Primitive::Triangles, 0, 3);
  }
  void clear(const gx::EfbCopy& c) {
    ensure_efb_pass();
    if (!efb_pass_) return;
    const uint32_t colour = gx::look_clear_colour(lightweight_filter_.in_match, c.clear_color);   // the look's background
    ClearParams params{};
    params.color[0] = ((colour >> 16) & 255) / 255.0f; params.color[1] = ((colour >> 8) & 255) / 255.0f;
    params.color[2] = (colour & 255) / 255.0f; params.color[3] = (colour >> 24) / 255.0f;
    params.depth = float(1.0 - c.clear_z / 16777215.0);
    dev_->set_pipeline(internal_pipeline("clear_fs", mtl::Format::RGBA8, mtl::Format::Depth32F, msaa_samples_));
    dev_->set_depth_state(dev_->depth_state(true, mtl::Compare::Always, true));
    dev_->set_cull(mtl::Cull::None, true);
    dev_->set_fill_lines(false);
    dev_->set_viewport(0, 0, float(width_), float(height_), 0, 1);
    dev_->set_scissor(int(c.src_x) * scale_, int(c.src_y) * scale_, int(c.src_w) * scale_, int(c.src_h) * scale_);
    dev_->set_fragment_bytes(&params, sizeof params, 0);
    dev_->draw(mtl::Primitive::Triangles, 0, 3);
  }
  void copy(const gx::EfbCopy& c) {
    if (!c.src_w || !c.src_h) return;
    end_efb_pass();   // resolves a multisampled EFB
    const uint32_t w = std::max(1u, c.src_w / (c.half_scale ? 2 : 1)) * scale_, h = std::max(1u, c.src_h / (c.half_scale ? 2 : 1)) * scale_;
    auto& e = copies_[c.dest_addr & 0x3FFFFFFFu];
    if (!e.tex || e.width != w || e.height != h) {
      dev_->release(e.tex);
      e = {}; e.width = w; e.height = h;
      e.tex = dev_->new_texture(w, h, 1, mtl::Format::RGBA8, true, 1, "EFB copy");
      if (!e.tex) throw std::runtime_error("Metal: cannot allocate an EFB copy");
    }
    e.used = sequence_;
    mtl::PassDesc d; d.color = e.tex; d.color_load = mtl::Load::DontCare;
    if (!dev_->begin_pass(d)) return;
    dev_->set_viewport(0, 0, float(w), float(h), 0, 1);
    dev_->set_scissor(0, 0, int(w), int(h));
    CopyParams params{};
    params.rect[0] = c.src_w / float(gx::EFB_WIDTH); params.rect[1] = c.src_h / float(gx::EFB_HEIGHT);
    params.rect[2] = c.src_x / float(gx::EFB_WIDTH); params.rect[3] = c.src_y / float(gx::EFB_HEIGHT);
    params.kind = c.efb_alpha ? 0 : 1;
    if (c.is_depth)
      quad(internal_pipeline("copy_depth_fs", mtl::Format::RGBA8, mtl::Format::None, 1), efb_depth_, nearest_sampler_, &params, sizeof params);
    else
      quad(internal_pipeline("copy_fs", mtl::Format::RGBA8, mtl::Format::None, 1), efb_color_, linear_sampler_, &params, sizeof params);
    dev_->end_pass();
  }
  void present(const gx::EfbCopy& c, const gx::Frame& frame) {
    const int test_fill = frame.test_fill;
    if (present_pending_) {   // a second picture in one frame: show the first, then go on
      dev_->end_frame(true); efb_pass_ = false; after_present(frame);
      dev_->begin_frame();
      vertices_ = rebind_vertices(frame);
      present_pending_ = false;
    }
    end_efb_pass();
    int w = 0, h = 0;
    if (!dev_->drawable_size(&w, &h)) return;
    if (w != drawable_w_ || h != drawable_h_) {
      host::log("Mac video: Metal drawable %dx%d", w, h);
      drawable_w_ = w; drawable_h_ = h;
    }
    dev_->copy_texture(efb_color_, last_color_);
    have_last_ = true;
    const float fill = test_fill > 0 ? 1.0f : 0.0f;
    mtl::PassDesc d; d.drawable = true; d.color_load = mtl::Load::Clear;
    d.clear_color[0] = d.clear_color[1] = d.clear_color[2] = fill; d.clear_color[3] = 1;
    if (!dev_->begin_pass(d)) return;
    if (test_fill < 0 && hooks_.options) border_art(w, h);
    if (test_fill < 0) {
      const double aspect = hooks_.options ? gx::presented_aspect(*hooks_.options, w, h, widenable_scene_) : 73.0 / 60.0;
      int pw = w, ph = int(std::lround(w / aspect));
      if (ph > h) { ph = h; pw = int(std::lround(h * aspect)); }
      picture(c, w, h, pw, ph);
    }
    if (hooks_.before_swap) {
      dev_->set_viewport(0, 0, float(w), float(h), 0, 1);
      dev_->set_scissor(0, 0, w, h);
      hooks_.before_swap(w, h);
      dev_->forget_state();   // the overlay drew through the encoder with its own state
    }
    if (test_fill < 0) { ++presented_; captures(capture_sequence_, w, h); --presented_; }   // the finished window, menus included
    test_shot(w, h);
    dev_->end_pass();
    present_pending_ = true;
    present_fill_ = test_fill;
  }
  void after_present(const gx::Frame&) {
    ++presented_;
    if (hooks_.after_swap) hooks_.after_swap(presented_);
    // Ready = the game drew its first real frame, visible or not (the YouGame page's loading cover).
    yougame::parity::signals.presented(present_fill_ < 0);
  }
  mtl::Span rebind_vertices(const gx::Frame& frame) {
    // A new frame of the ring after a mid-frame present: the vertex stream goes in again.
    mtl::Span v = dev_->alloc(std::max<size_t>(frame.vertices.size(), 1) * sizeof(gx::Vertex), 16);
    if (!frame.vertices.empty()) std::memcpy(v.cpu, frame.vertices.data(), frame.vertices.size() * sizeof(gx::Vertex));
    return v;
  }
  void resume_drawable_pass() {
    mtl::PassDesc d; d.drawable = true; d.color_load = mtl::Load::Load;
    dev_->begin_pass(d);
  }
  // The picture on the window (see picture_fs).
  void picture(const gx::EfbCopy& c, int w, int h, int pw, int ph) {
    const gx::RenderOptions* o = hooks_.options;
    const gx::CrtLook look = gx::crt_look(o ? o->crt_filter : 0, o ? o->crt_curve : 1.0f);
    const float lines = std::max(float(c.src_h) * c.y_scale, 1.0f);
    const bool crt_on = o && o->crt_filter;
    float vh = float(ph), vw = float(pw);
    gx::CrtFit fit{vh, 0.0f, lines};
    // Only the built-in CRT model lays its lines on whole pixels (crt_fit: more, finer lines, no border).
    if (crt_on) fit = gx::crt_fit(vh, lines);
    const float vx = std::floor((w - vw) * 0.5f), vy = std::floor((h - vh) * 0.5f);
    dev_->set_viewport(vx, vy, std::max(vw, 1.0f), std::max(vh, 1.0f), 0, 1);
    dev_->set_scissor(0, 0, w, h);
    PictureParams u{};
    const float rect[4] = {c.src_w / float(gx::EFB_WIDTH), c.src_h / float(gx::EFB_HEIGHT), c.src_x / float(gx::EFB_WIDTH), c.src_y / float(gx::EFB_HEIGHT)};
    std::memcpy(u.rect, rect, sizeof rect);
    u.sharp[0] = 1.0f / width_; u.sharp[1] = 1.0f / height_; u.sharp[2] = o ? std::clamp(o->sharpness, 0.0f, 1.0f) : 0.0f;
    u.box[0] = float(std::clamp(int(std::lround(double(c.src_w) * scale_ / std::max(vw, 1.0f))), 1, 4));
    u.box[1] = float(std::clamp(int(std::lround(double(c.src_h) * scale_ / std::max(vh, 1.0f))), 1, 4));
    u.col[0] = o ? o->brightness : 1.0f; u.col[1] = o ? o->contrast : 1.0f; u.col[2] = o ? o->vibrance : 1.0f;
    u.crt[0] = look.scan; u.crt[1] = look.mask; u.crt[2] = look.curve; u.crt[3] = fit.lines;
    u.crt2[0] = float(c.src_w); u.crt2[1] = vh / fit.lines; u.crt2[2] = look.halation; u.crt2[3] = fit.shift;
    u.crt_on = crt_on ? 1 : 0;
    quad(internal_pipeline("picture_fs", mtl::Format::BGRA8, mtl::Format::None, 1), efb_color_, linear_sampler_, &u, sizeof u);
  }
  // MELEE_TEST_SHOT=<dir>:<n>,<n>,...: save the picture presented to the window at those presents, as
  // PPM; <dir>:all appends "<present> <hash>" for every present to <dir>/hashes.txt (renderer_gl.cpp).
  void test_shot(int w, int h) {
    static std::filesystem::path dir; static std::vector<uint64_t> at; static bool parsed = false, all = false;
    static std::ofstream hashes;
    if (!parsed) {
      parsed = true;
      if (const char* v = host::test_env::getenv("MELEE_TEST_SHOT")) {
        std::string spec(v); const auto colon = spec.rfind(':');
        if (colon != std::string::npos) {
          dir = spec.substr(0, colon);
          if (spec.compare(colon + 1, std::string::npos, "all") == 0) { all = true; hashes.open(dir / "hashes.txt"); }
          else for (size_t i = colon + 1; i < spec.size();) { size_t j = spec.find(',', i); if (j == std::string::npos) j = spec.size();
            at.push_back(std::strtoull(spec.substr(i, j - i).c_str(), nullptr, 10)); i = j + 1; }
        }
      }
    }
    if (!all && (at.empty() || std::find(at.begin(), at.end(), presented_) == at.end())) return;
    std::vector<uint8_t> rgba;
    if (!dev_->read_pixels(nullptr, 0, 0, w, h, rgba)) return;
    resume_drawable_pass();
    if (all) {
      std::vector<uint8_t> rgb(size_t(w) * h * 3);
      for (size_t i = 0, n = size_t(w) * h; i < n; ++i) for (int k = 0; k < 3; ++k) rgb[i * 3 + k] = rgba[i * 4 + k];
      hashes << presented_ << ' ' << std::hex << gx::hash_bytes(rgb.data(), rgb.size()) << std::dec << std::endl;
      return;
    }
    write_ppm(dir / ("present-" + std::to_string(presented_) + ".ppm"), rgba, w, h);
  }
  // A video frame in its slot's texture (re-uploaded only when the decoder has a new one).
  mtl::Texture video_texture(const std::shared_ptr<const gx::video_bg::Frame>& f, int slot) {
    if (!f || slot < 0 || slot >= gx::video_bg::kVideoSlots || f->bgra.empty()) return nullptr;
    VideoTexture& v = video_[slot];
    if (!v.tex || v.width != f->width || v.height != f->height) {
      dev_->release(v.tex);
      v.tex = dev_->new_texture(f->width, f->height, 1, mtl::Format::BGRA8, false);
      v.width = f->width; v.height = f->height; v.serial = 0;
    }
    if (v.serial != f->serial) {
      dev_->upload(v.tex, 0, f->width, f->height, f->bgra.data(), size_t(f->width) * 4);
      v.serial = f->serial;
    }
    return v.tex;
  }
  void draw_video_layer(const std::shared_ptr<const gx::video_bg::Frame>& f, int slot, const gx::EfbCopy& screen) {
    const mtl::Texture tex = video_texture(f, slot);
    if (!tex) return;
    ensure_efb_pass();
    if (!efb_pass_) return;
    const float x = float(screen.src_x * scale_), y = float(screen.src_y * scale_);
    const float w = float(screen.src_w * scale_), h = float(screen.src_h * scale_);
    dev_->set_viewport(x, y, w, h, 0, 1);
    dev_->set_scissor(int(x), int(y), int(w), int(h));
    // The decoder hands the rows bottom first, as on Windows: v runs from 1 at the top.
    CopyParams params{};
    params.rect[0] = 1.0f; params.rect[1] = -1.0f; params.rect[2] = 0.0f; params.rect[3] = 1.0f; params.kind = 1;
    quad(internal_pipeline("copy_fs", mtl::Format::RGBA8, mtl::Format::Depth32F, msaa_samples_), tex, linear_sampler_, &params, sizeof params);
    if (!video_layer_logged_[slot & 1]) {
      video_layer_logged_[slot & 1] = true;
      std::fprintf(stderr, "video backgrounds: %s full-screen layer drawn (Metal)\n", slot ? "SSS" : "CSS");
    }
  }
  // Border art (Video settings) fills the window behind the picture, cropped to the window's shape.
  void border_art(int w, int h) {
    gx::video_bg::set_border(hooks_.options->border_art);
    bool reversed = false;
    const auto art = gx::video_bg::border_frame(&reversed);
    if (!art) return;
    const mtl::Texture tex = video_texture(art, gx::video_bg::kBorderSlot);
    if (!tex) return;
    const float window = float(w) / std::max(float(h), 1.0f), shape = float(art->width) / std::max(float(art->height), 1.0f);
    const float sx = shape > window ? window / shape : 1.0f, sy = shape > window ? 1.0f : shape / window;
    CopyParams params{};
    params.rect[0] = sx; params.rect[1] = reversed ? -sy : sy;
    params.rect[2] = (1.0f - sx) * 0.5f; params.rect[3] = reversed ? 1.0f - (1.0f - sy) * 0.5f : (1.0f - sy) * 0.5f;
    params.kind = 1;
    dev_->set_viewport(0, 0, float(w), float(h), 0, 1);
    dev_->set_scissor(0, 0, w, h);
    quad(internal_pipeline("copy_fs", mtl::Format::BGRA8, mtl::Format::None, 1), tex, linear_sampler_, &params, sizeof params);
  }
  void trim() {
    if (sequence_ % 120) return;
    for (auto i = textures_.begin(); i != textures_.end();) {
      if (i->second.used + 180 < sequence_) { release(i->second); i = textures_.erase(i); } else ++i;
    }
    for (auto i = copies_.begin(); i != copies_.end();) {
      if (i->second.used + 3600 < sequence_) { dev_->release(i->second.tex); i = copies_.erase(i); } else ++i;
    }
    uint64_t bytes = 0;
    for (const auto& item : textures_) bytes += item.second.bytes;
    constexpr uint64_t budget = 256ull << 20;
    if (bytes > budget) {
      std::vector<std::pair<uint64_t, uint64_t>> oldest;
      for (const auto& item : textures_)
        if (item.second.used != sequence_) oldest.emplace_back(item.second.used, item.first);
      std::sort(oldest.begin(), oldest.end());
      for (const auto& item : oldest) {
        if (bytes <= budget * 9 / 10) break;
        auto i = textures_.find(item.second);
        bytes -= i->second.bytes; release(i->second); textures_.erase(i);
      }
    }
  }

  SDL_Window* window_;
  std::filesystem::path cache_;
  std::filesystem::path seed_;   // MELEE_SHADER_SEED's gx-metal-v1, or empty
  std::unique_ptr<mtl::Device> dev_;
  Hooks hooks_;
  int scale_, width_, height_;
  bool vsync_;
  bool skip_ = false, in_frame_ = false, present_pending_ = false;
  int present_fill_ = -1;
  uint64_t sequence_ = 0;
  uint32_t presented_ = 0, source_retrace_ = 0, capture_frame_ = 0;
  uint64_t capture_sequence_ = 0;
  int drawable_w_ = 0, drawable_h_ = 0;
  // EFB
  mtl::Texture efb_color_ = nullptr, efb_depth_ = nullptr, last_color_ = nullptr;
  mtl::Texture msaa_color_ = nullptr, msaa_depth_ = nullptr;
  uint32_t msaa_samples_ = 1;
  bool efb_pass_ = false, efb_fresh_ = true, have_last_ = false;
  mtl::Span vertices_;
  // Internal passes
  std::unordered_map<std::string, mtl::Function> internal_;
  std::unordered_map<std::string, mtl::Pipeline> internal_pipelines_;
  mtl::Sampler linear_sampler_ = nullptr, nearest_sampler_ = nullptr;
  mtl::Texture white_ = nullptr;   // "Lightweight" flat shading
  // GX pipelines
  std::unordered_map<ProgramKey, std::shared_ptr<Program>, ProgramKeyHash> programs_;
  std::unordered_map<uint64_t, mtl::Pipeline> pipelines_;
  std::unordered_set<uint64_t> pending_, failed_;
  std::vector<ProgramRecipe> recipes_;
  std::unordered_set<uint64_t> recipe_hashes_;
  size_t saved_recipes_ = 0;
  bool prewarming_ = false, prewarmed_ = false;
  const bool sync_shaders_ = [] { const char* v = std::getenv("MELEE_SYNC_SHADERS"); return v && std::strcmp(v, "1") == 0; }();
  std::vector<std::thread> workers_;
  std::mutex jobs_mutex_, results_mutex_;
  std::condition_variable jobs_cv_, results_cv_;
  std::deque<Job> jobs_;
  std::deque<Result> results_;
  bool stopping_ = false;
  std::atomic<size_t> translated_{0};
  uint64_t skipped_draws_ = 0, skipped_draws_total_ = 0;
  Clock::time_point skip_report_ = Clock::now();
  // Textures
  std::unordered_map<uint64_t, Texture> textures_;
  std::unordered_map<uint32_t, Texture> copies_;
  std::unordered_map<uint64_t, mtl::Sampler> samplers_;
  struct VideoTexture { mtl::Texture tex = nullptr; uint32_t width = 0, height = 0; uint64_t serial = 0; };
  VideoTexture video_[gx::video_bg::kVideoSlots];
  bool video_layer_logged_[2] = {false, false};
  int draw_video_slot_ = -1;   // the video slot the last draw sampled (the stage select's layer follows it)
  uint64_t replacement_bytes_ = 0;   // texture pack replacements held, against a 512 MB budget
  int anisotropy_applied_ = 1;
  gx::EffectsFilter effects_filter_;
  gx::LightweightFilter lightweight_filter_;
  gx::Surface look_surface_ = gx::Surface::Textured;
  bool widenable_scene_ = true;
  bool clockwise_front_ = [] { const char* v = std::getenv("MELEE_METAL_CCW"); return !(v && *v == '1'); }();
  gx::VSConstants vc_{};
  std::vector<uint8_t> decoded_;
  std::vector<uint32_t> index_scratch_;
  std::vector<size_t> vertex_offsets_;
};

}  // namespace

// renderer_gl.h's factory. A prewarm-only run (main.cpp run_prewarm_only, the launcher's shader pass on
// Windows and Linux) is not one the Mac app starts: the Metal renderer prewarms at its own start.
std::unique_ptr<gx::Backend> create(SDL_Window* window, const std::filesystem::path& cache, int scale, bool vsync, bool /*prewarm_only*/) {
  return std::make_unique<Renderer>(window, cache, scale, vsync);
}
void capture_efb(gx::Backend& backend, const std::filesystem::path& path) { dynamic_cast<Renderer&>(backend).capture(path); }
void capture_culling_variants(gx::Backend& backend, const gx::Frame& frame, const std::filesystem::path& path) {
  dynamic_cast<Renderer&>(backend).compare_culling(frame, path);
}
void set_hooks(gx::Backend& backend, Hooks hooks) { dynamic_cast<Renderer&>(backend).set_hooks(std::move(hooks)); }
void* metal_native_device(gx::Backend& backend) { return dynamic_cast<Renderer&>(backend).native_device(); }
MetalOverlay metal_overlay(gx::Backend& backend) { return dynamic_cast<Renderer&>(backend).overlay(); }

// The renderer's self-test (--graphics-self-test): a vertex-coloured triangle and a textured one on
// texture unit 7, through the real shader generator, translator and Metal path.
bool smoke_test(gx::Backend& backend, const std::filesystem::path& capture) {
  gx::Frame frame; gx::DrawCall dc{};
  dc.primitive = 0x90; dc.vertex_count = 3; dc.components = gx::VB_HAS_COL0;
  dc.bp.reg[gx::BP_BLENDMODE] = 8 | 16;
  dc.bp.reg[gx::BP_SCISSORTL] = (342 << 12) | 342; dc.bp.reg[gx::BP_SCISSORBR] = (981 << 12) | 869;
  dc.bp.reg[gx::BP_SCISSOROFFSET] = 171 | (171 << 10);
  dc.bp.reg[gx::BP_GENMODE] = 0;
  dc.bp.reg[gx::BP_TEV_COLOR_ENV] = (15 << 12) | (15 << 8) | (15 << 4) | 10 | (1 << 19);
  dc.bp.reg[gx::BP_TEV_ALPHA_ENV] = (7 << 13) | (7 << 10) | (7 << 7) | (5 << 4) | (1 << 19);
  dc.bp.reg[gx::BP_ALPHACOMPARE] = (7 << 16) | (7 << 19);
  dc.bp.reg[gx::BP_TEV_KSEL] = 0 | (1 << 2);
  dc.bp.reg[gx::BP_TEV_KSEL + 1] = 2 | (3 << 2);
  dc.xf_regs[0x08] = 0; dc.xf_regs[0x09] = 0;
  float viewport[6] = {320, -264, 16777216, 662, 606, 16777216};
  std::memcpy(&dc.xf_regs[0x1A], viewport, sizeof viewport);
  float projection[7] = {1, 0, 1, 0, 0, -0.5f, 1};
  std::memcpy(&dc.xf_regs[0x20], projection, sizeof projection);
  dc.posMatrices[0] = dc.posMatrices[5] = dc.posMatrices[10] = 1;
  const float positions[3][3] = {{-0.8f, -0.7f, 0}, {0.8f, -0.7f, 0}, {0, 0.7f, 0}};
  for (int i = 0; i < 3; ++i) { gx::Vertex v{}; std::memcpy(v.pos, positions[i], sizeof v.pos); v.col0[i] = 255; v.col0[3] = 255; frame.vertices.push_back(v); }
  frame.draws.push_back(dc); frame.commands.push_back({gx::FrameCommand::Draw, 0});
  gx::DrawCall textured = dc;
  textured.first_vertex = 3; textured.components |= gx::VB_HAS_UV0;
  textured.bp.reg[gx::BP_GENMODE] = 1;
  textured.bp.reg[gx::BP_TREF] = 7 | (1 << 6);
  textured.bp.reg[gx::BP_TEV_COLOR_ENV] = (15 << 12) | (15 << 8) | (15 << 4) | 8 | (1 << 19);
  textured.bp.reg[gx::BP_TEV_ALPHA_ENV] = (7 << 13) | (7 << 10) | (7 << 7) | (4 << 4) | (1 << 19);
  textured.xf_regs[0x3F] = 1;
  textured.xf_regs[0x40] = 5 << 7;
  textured.matrix_index_a = 60 << 6;
  auto pixels = std::make_shared<gx::TextureSnapshot>();
  pixels->image.assign(32, 200); pixels->hash = gx::hash_bytes(pixels->image.data(), pixels->image.size());
  auto& t = textured.textures[7]; t.used = true; t.addr = 0x1000; t.width = 8; t.height = 4; t.format = 1; t.data = pixels;
  for (int i = 0; i < 3; ++i) {
    auto v = frame.vertices[i]; v.pos[0] = v.pos[0] * 0.25f + 0.6f; v.pos[1] *= 0.25f;
    v.texmtx[0] = 60; v.uv[0][0] = i == 1 ? 1 : 0; v.uv[0][1] = i == 2 ? 1 : 0;
    frame.vertices.push_back(v);
  }
  frame.draws.push_back(textured); frame.commands.push_back({gx::FrameCommand::Draw, 1});
  gx::EfbCopy present{}; present.to_xfb = true; present.src_w = 640; present.src_h = 528; present.y_scale = 1;
  frame.copies.push_back(present); frame.commands.push_back({gx::FrameCommand::Copy, 0});
  auto& renderer = dynamic_cast<Renderer&>(backend);
  bool matched = false;
  const auto deadline = Clock::now() + std::chrono::seconds(45);
  do {
    backend.submit_frame(frame);
    renderer.finish_builds(5000);
    backend.submit_frame(frame);
    matched = renderer.check_smoke_pixels();
    if (matched) break;
    SDL_PumpEvents(); SDL_Delay(10);
  } while (Clock::now() < deadline);
  capture_efb(backend, capture);
  if (!matched) throw std::runtime_error("Metal GX smoke pixels did not match vertex-colour and unit-7 texture draws");
  return true;
}

}  // namespace linux_renderer
