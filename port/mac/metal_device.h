// A small Metal layer for the Mac renderer (renderer_metal.cpp) and the settings overlay.
//
// The renderer itself is ordinary C++ built with the Win32 compatibility headers the rest of the host
// uses; those typedef BOOL and friends in ways AppKit and Metal cannot see. So nothing here names an
// Objective-C type: every Metal object is an opaque handle, and metal_device.mm (Objective-C++, no
// compatibility headers) is the only file that talks to Metal.
//
// One Device per window. All calls except the shader/pipeline builders (marked) belong to the thread
// that renders. A frame is begin_frame() ... end_frame(): its uploads live in that frame's slot of a
// ring that is reused once the GPU has finished with it (three frames in flight).
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct SDL_Window;

namespace mtl {

struct TextureObject; struct SamplerObject; struct FunctionObject; struct PipelineObject; struct DepthObject;
struct BufferObject;
using Texture = TextureObject*;
using Sampler = SamplerObject*;
using Function = FunctionObject*;
using Pipeline = PipelineObject*;
using DepthState = DepthObject*;

enum class Format : uint8_t { None, RGBA8, BGRA8, Depth32F };
enum class Compare : uint8_t { Never, Less, Equal, LessEqual, Greater, NotEqual, GreaterEqual, Always };
enum class Factor : uint8_t {
  Zero, One, SrcColor, OneMinusSrcColor, SrcAlpha, OneMinusSrcAlpha, DstAlpha, OneMinusDstAlpha,
  DstColor, OneMinusDstColor
};
enum class BlendOp : uint8_t { Add, ReverseSubtract };
enum class Cull : uint8_t { None, Front, Back };
enum class Primitive : uint8_t { Triangles, Lines };
enum class Filter : uint8_t { Nearest, Linear };
enum class MipFilter : uint8_t { None, Nearest, Linear };
enum class Wrap : uint8_t { Clamp, Repeat, Mirror };
enum class VertexFormat : uint8_t { Float2, Float3, UChar4Normalized, UChar4, UChar };
enum class Load : uint8_t { Load, Clear, DontCare };

struct SamplerDesc {
  Filter min = Filter::Linear, mag = Filter::Linear;
  MipFilter mip = MipFilter::None;
  Wrap s = Wrap::Clamp, t = Wrap::Clamp;
  float min_lod = 0, max_lod = 1000;
  int anisotropy = 1;
};

struct VertexAttribute { uint32_t location; VertexFormat format; uint32_t offset; };

struct BlendDesc {
  bool enabled = false;
  Factor src = Factor::One, dst = Factor::Zero, src_alpha = Factor::One, dst_alpha = Factor::Zero;
  BlendOp op = BlendOp::Add, op_alpha = BlendOp::Add;
  bool write_rgb = true, write_alpha = true;
};

struct PipelineDesc {
  Function vertex = nullptr, fragment = nullptr;
  std::vector<VertexAttribute> attributes;   // empty: no vertex buffer (the full-screen passes)
  uint32_t vertex_stride = 0;
  Format color = Format::RGBA8, depth = Format::None;
  uint32_t samples = 1;
  BlendDesc blend;
  const char* label = nullptr;
};

// The vertex buffer's slot in every GX vertex function (above the constant buffers).
constexpr int kVertexBufferIndex = 30;

// A piece of the current frame's upload ring. `cpu` stays writable until end_frame().
struct Span { void* cpu = nullptr; BufferObject* buffer = nullptr; size_t offset = 0; };

struct PassDesc {
  Texture color = nullptr, depth = nullptr;
  Texture resolve_color = nullptr, resolve_depth = nullptr;   // multisampled targets resolve into these
  Load color_load = Load::Load, depth_load = Load::Load;
  float clear_color[4] = {0, 0, 0, 1};
  double clear_depth = 0;
  bool drawable = false;   // the window's next drawable instead of `color`
};

class Device {
 public:
  // nullptr with the reason in `error` when there is no Metal device or the window has no Metal view.
  static std::unique_ptr<Device> create(SDL_Window* window, bool vsync, std::string* error);
  virtual ~Device() = default;

  virtual std::string name() const = 0;
  virtual uint32_t supported_samples(uint32_t wanted) const = 0;
  virtual void set_vsync(bool on) = 0;
  // The window's drawable in pixels; the layer is resized to it here.
  virtual bool drawable_size(int* w, int* h) = 0;
  virtual void* native_device() const = 0;   // id<MTLDevice>, for the ImGui backend

  // Resources. Textures, samplers and depth states: render thread.
  virtual Texture new_texture(uint32_t w, uint32_t h, uint32_t levels, Format format, bool render_target,
                              uint32_t samples = 1, const char* label = nullptr) = 0;
  virtual void upload(Texture texture, uint32_t level, uint32_t w, uint32_t h, const void* rgba, size_t row_bytes) = 0;
  virtual void release(Texture texture) = 0;
  virtual uint32_t width(Texture texture) const = 0;
  virtual uint32_t height(Texture texture) const = 0;
  virtual Sampler new_sampler(const SamplerDesc& desc) = 0;
  virtual void release(Sampler sampler) = 0;
  virtual DepthState depth_state(bool test, Compare compare, bool write) = 0;   // cached, never released

  // Shader functions and pipelines: any thread (Metal device objects are thread safe).
  virtual Function new_function(const std::string& msl, const std::string& entry, std::string* error) = 0;
  virtual void release(Function function) = 0;
  virtual Pipeline new_pipeline(const PipelineDesc& desc, std::string* error) = 0;
  virtual void release(Pipeline pipeline) = 0;

  // A frame. begin_frame waits for a free slot of the ring (the GPU three frames behind at most).
  virtual void begin_frame() = 0;
  virtual Span alloc(size_t bytes, size_t align = 256) = 0;
  // Ends any open pass and starts one on these attachments. drawable: false when the window has no
  // drawable to give (minimised, zero size); nothing is drawn until the next begin_pass then.
  virtual bool begin_pass(const PassDesc& desc) = 0;
  virtual void end_pass() = 0;
  virtual bool in_pass() const = 0;
  // Presents the drawable used this frame (if any) and commits. The ring slot is free once the GPU
  // has run it.
  virtual void end_frame(bool present) = 0;

  // State inside a pass (redundant calls are dropped).
  virtual void set_pipeline(Pipeline pipeline) = 0;
  virtual void set_depth_state(DepthState state) = 0;
  virtual void set_viewport(float x, float y, float w, float h, float znear, float zfar) = 0;
  virtual void set_scissor(int x, int y, int w, int h) = 0;   // clamped to the pass's size
  virtual void set_cull(Cull cull, bool clockwise_front) = 0;
  virtual void set_fill_lines(bool lines) = 0;
  virtual void set_vertex_buffer(const Span& span, int index) = 0;
  virtual void set_fragment_buffer(const Span& span, int index) = 0;
  virtual void set_vertex_bytes(const void* data, size_t bytes, int index) = 0;     // up to 4 KB
  virtual void set_fragment_bytes(const void* data, size_t bytes, int index) = 0;   // up to 4 KB
  virtual void set_fragment_texture(Texture texture, int index) = 0;
  virtual void set_fragment_sampler(Sampler sampler, int index) = 0;
  virtual void draw_indexed(Primitive primitive, uint32_t count, const Span& indices) = 0;   // uint32 indices
  virtual void draw(Primitive primitive, uint32_t first, uint32_t count) = 0;

  // Outside a pass: copy a whole level-0 texture into another of the same size and format.
  virtual void copy_texture(Texture from, Texture to) = 0;
  // Reads a texture (or, with nullptr, the drawable of the current frame) back as RGBA8, rows top to
  // bottom. Finishes the GPU work so far: slow, for captures and tests only. Ends any open pass.
  virtual bool read_pixels(Texture texture, int x, int y, int w, int h, std::vector<uint8_t>& rgba) = 0;

  // The current frame's command buffer, render encoder and pass descriptor (for the ImGui backend).
  virtual void* native_command_buffer() = 0;
  virtual void* native_encoder() = 0;
  virtual void* native_pass_descriptor() = 0;
  // ImGui draws through its own encoder state: after it, every cached state is unknown.
  virtual void forget_state() = 0;
};

}  // namespace mtl
