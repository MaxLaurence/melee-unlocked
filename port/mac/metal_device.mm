// Metal for the Mac renderer: see metal_device.h. Built with ARC and without the Win32 compatibility
// headers, so AppKit, QuartzCore and Metal see their own types.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "metal_device.h"
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <AppKit/AppKit.h>
#include <SDL.h>
#include <SDL_metal.h>
#include <SDL_syswm.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>

#if !__has_feature(objc_arc)
#error "metal_device.mm needs ARC (-fobjc-arc)"
#endif

extern "C" void* objc_autoreleasePoolPush(void);
extern "C" void objc_autoreleasePoolPop(void*);

namespace mtl {
namespace {

// Handles are retained Objective-C objects (bridged out of ARC); release() hands them back.
template <class H> H out(id object) { return reinterpret_cast<H>((__bridge_retained void*)object); }
template <class T, class H> T in(H handle) { return (__bridge T)reinterpret_cast<void*>(handle); }
template <class H> void drop(H handle) { if (handle) { id object = (__bridge_transfer id)reinterpret_cast<void*>(handle); object = nil; } }

MTLPixelFormat pixel_format(Format f) {
  switch (f) {
    case Format::RGBA8: return MTLPixelFormatRGBA8Unorm;
    case Format::BGRA8: return MTLPixelFormatBGRA8Unorm;
    case Format::Depth32F: return MTLPixelFormatDepth32Float;
    default: return MTLPixelFormatInvalid;
  }
}
MTLCompareFunction compare(Compare c) {
  static const MTLCompareFunction table[] = {MTLCompareFunctionNever, MTLCompareFunctionLess, MTLCompareFunctionEqual,
      MTLCompareFunctionLessEqual, MTLCompareFunctionGreater, MTLCompareFunctionNotEqual,
      MTLCompareFunctionGreaterEqual, MTLCompareFunctionAlways};
  return table[static_cast<int>(c)];
}
MTLBlendFactor factor(Factor f) {
  static const MTLBlendFactor table[] = {MTLBlendFactorZero, MTLBlendFactorOne, MTLBlendFactorSourceColor,
      MTLBlendFactorOneMinusSourceColor, MTLBlendFactorSourceAlpha, MTLBlendFactorOneMinusSourceAlpha,
      MTLBlendFactorDestinationAlpha, MTLBlendFactorOneMinusDestinationAlpha, MTLBlendFactorDestinationColor,
      MTLBlendFactorOneMinusDestinationColor};
  return table[static_cast<int>(f)];
}
MTLVertexFormat vertex_format(VertexFormat f) {
  switch (f) {
    case VertexFormat::Float2: return MTLVertexFormatFloat2;
    case VertexFormat::Float3: return MTLVertexFormatFloat3;
    case VertexFormat::UChar4Normalized: return MTLVertexFormatUChar4Normalized;
    case VertexFormat::UChar4: return MTLVertexFormatUChar4;
    case VertexFormat::UChar: return MTLVertexFormatUChar;
  }
  return MTLVertexFormatInvalid;
}
MTLSamplerAddressMode address(Wrap w) {
  return w == Wrap::Repeat ? MTLSamplerAddressModeRepeat : w == Wrap::Mirror ? MTLSamplerAddressModeMirrorRepeat
                                                                              : MTLSamplerAddressModeClampToEdge;
}
MTLLoadAction load_action(Load l) {
  return l == Load::Clear ? MTLLoadActionClear : l == Load::DontCare ? MTLLoadActionDontCare : MTLLoadActionLoad;
}
NSString* ns(const char* s) { return s ? [NSString stringWithUTF8String:s] : nil; }

constexpr int kSlots = 3;
constexpr size_t kRingBlock = 32u << 20;

class MetalDevice final : public Device {
 public:
  MetalDevice(SDL_Window* window, id<MTLDevice> device, SDL_MetalView view, CAMetalLayer* layer, bool vsync)
      : window_(window), device_(device), view_(view), layer_(layer) {
    queue_ = [device_ newCommandQueue];
    queue_.label = @"Melee Unlocked";
    inflight_ = dispatch_semaphore_create(kSlots);
    layer_.device = device_;
    layer_.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer_.framebufferOnly = NO;   // captures and tests read the window back
    layer_.displaySyncEnabled = vsync ? YES : NO;
    if (const char* n = std::getenv("MELEE_METAL_DRAWABLES")) {
      const int count = std::atoi(n);
      if (count == 2 || count == 3) layer_.maximumDrawableCount = count;
    }
    unified_ = device_.hasUnifiedMemory;
    drawable_size(nullptr, nullptr);
  }
  ~MetalDevice() override {
    if (command_buffer_) end_frame(false);
    // Wait for every frame the GPU still has.
    for (int i = 0; i < kSlots; ++i) dispatch_semaphore_wait(inflight_, DISPATCH_TIME_FOREVER);
    for (int i = 0; i < kSlots; ++i) dispatch_semaphore_signal(inflight_);
    for (auto& d : depth_states_) drop(d.second);
    if (view_) SDL_Metal_DestroyView(view_);
  }

  std::string name() const override { return device_.name.UTF8String; }
  uint32_t supported_samples(uint32_t wanted) const override {
    for (uint32_t n = std::min<uint32_t>(wanted, 8); n > 1; n /= 2)
      if ([device_ supportsTextureSampleCount:n]) return n;
    return 1;
  }
  void set_vsync(bool on) override { layer_.displaySyncEnabled = on ? YES : NO; }
  bool drawable_size(int* w, int* h) override {
    int dw = 0, dh = 0;
    SDL_Metal_GetDrawableSize(window_, &dw, &dh);
    if (dw > 0 && dh > 0 && (layer_.drawableSize.width != dw || layer_.drawableSize.height != dh))
      layer_.drawableSize = CGSizeMake(dw, dh);
    if (w) *w = dw;
    if (h) *h = dh;
    return dw > 0 && dh > 0;
  }
  void* native_device() const override { return (__bridge void*)device_; }

  Texture new_texture(uint32_t w, uint32_t h, uint32_t levels, Format format, bool render_target, uint32_t samples,
                      const char* label) override {
    @autoreleasepool {
      MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pixel_format(format)
                                                                                    width:std::max(w, 1u)
                                                                                   height:std::max(h, 1u)
                                                                                mipmapped:NO];
      d.mipmapLevelCount = std::max(levels, 1u);
      d.usage = MTLTextureUsageShaderRead | (render_target ? MTLTextureUsageRenderTarget : 0);
      if (samples > 1) {
        d.textureType = MTLTextureType2DMultisample;
        d.sampleCount = samples;
        d.usage = MTLTextureUsageRenderTarget;
        d.mipmapLevelCount = 1;
      }
      d.storageMode = (render_target || format == Format::Depth32F) ? MTLStorageModePrivate
                     : unified_ ? MTLStorageModeShared : MTLStorageModeManaged;
      id<MTLTexture> t = [device_ newTextureWithDescriptor:d];
      if (!t) return nullptr;
      if (label) t.label = ns(label);
      return out<Texture>(t);
    }
  }
  void upload(Texture texture, uint32_t level, uint32_t w, uint32_t h, const void* rgba, size_t row_bytes) override {
    if (!texture || !rgba) return;
    id<MTLTexture> t = in<id<MTLTexture>>(texture);
    [t replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:level withBytes:rgba bytesPerRow:row_bytes];
  }
  void release(Texture texture) override {
    if (!texture) return;
    for (auto& bound : textures_) if (bound == texture) bound = nullptr;
    drop(texture);
  }
  uint32_t width(Texture texture) const override { return texture ? uint32_t(in<id<MTLTexture>>(texture).width) : 0; }
  uint32_t height(Texture texture) const override { return texture ? uint32_t(in<id<MTLTexture>>(texture).height) : 0; }
  Sampler new_sampler(const SamplerDesc& s) override {
    @autoreleasepool {
      MTLSamplerDescriptor* d = [MTLSamplerDescriptor new];
      d.minFilter = s.min == Filter::Linear ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
      d.magFilter = s.mag == Filter::Linear ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
      d.mipFilter = s.mip == MipFilter::Linear ? MTLSamplerMipFilterLinear
                  : s.mip == MipFilter::Nearest ? MTLSamplerMipFilterNearest : MTLSamplerMipFilterNotMipmapped;
      d.sAddressMode = address(s.s);
      d.tAddressMode = address(s.t);
      d.lodMinClamp = s.min_lod;
      d.lodMaxClamp = std::max(s.max_lod, s.min_lod);
      d.maxAnisotropy = std::clamp(s.anisotropy, 1, 16);
      return out<Sampler>([device_ newSamplerStateWithDescriptor:d]);
    }
  }
  void release(Sampler sampler) override {
    for (auto& bound : samplers_) if (bound == sampler) bound = nullptr;
    drop(sampler);
  }
  DepthState depth_state(bool test, Compare c, bool write) override {
    const int key = (test ? 1 : 0) | (write ? 2 : 0) | (static_cast<int>(c) << 2);
    auto found = depth_states_.find(key);
    if (found != depth_states_.end()) return found->second;
    @autoreleasepool {
      MTLDepthStencilDescriptor* d = [MTLDepthStencilDescriptor new];
      d.depthCompareFunction = test ? compare(c) : MTLCompareFunctionAlways;
      d.depthWriteEnabled = test && write;
      DepthState state = out<DepthState>([device_ newDepthStencilStateWithDescriptor:d]);
      depth_states_.emplace(key, state);
      return state;
    }
  }

  Function new_function(const std::string& msl, const std::string& entry, std::string* error) override {
    @autoreleasepool {
      MTLCompileOptions* options = [MTLCompileOptions new];
      // The GX maths is written to exact IEEE rules (Dolphin's integer TEV and depth): no fast maths.
      if (@available(macOS 15.0, *)) options.mathMode = MTLMathModeSafe;
      else options.fastMathEnabled = NO;
      options.languageVersion = MTLLanguageVersion2_3;
      NSError* err = nil;
      id<MTLLibrary> library = [device_ newLibraryWithSource:[NSString stringWithUTF8String:msl.c_str()]
                                                     options:options error:&err];
      if (!library) {
        if (error) *error = err ? err.localizedDescription.UTF8String : "Metal shader compile failed";
        return nullptr;
      }
      id<MTLFunction> function = [library newFunctionWithName:ns(entry.c_str())];
      if (!function) {
        if (error) *error = "Metal library has no function " + entry;
        return nullptr;
      }
      return out<Function>(function);
    }
  }
  void release(Function function) override { drop(function); }
  Pipeline new_pipeline(const PipelineDesc& p, std::string* error) override {
    @autoreleasepool {
      MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
      d.vertexFunction = in<id<MTLFunction>>(p.vertex);
      d.fragmentFunction = in<id<MTLFunction>>(p.fragment);
      if (p.label) d.label = ns(p.label);
      if (!p.attributes.empty()) {
        MTLVertexDescriptor* v = [MTLVertexDescriptor vertexDescriptor];
        for (const auto& a : p.attributes) {
          v.attributes[a.location].format = vertex_format(a.format);
          v.attributes[a.location].offset = a.offset;
          v.attributes[a.location].bufferIndex = kVertexBufferIndex;
        }
        v.layouts[kVertexBufferIndex].stride = p.vertex_stride;
        v.layouts[kVertexBufferIndex].stepFunction = MTLVertexStepFunctionPerVertex;
        d.vertexDescriptor = v;
      }
      MTLRenderPipelineColorAttachmentDescriptor* c = d.colorAttachments[0];
      c.pixelFormat = pixel_format(p.color);
      c.blendingEnabled = p.blend.enabled;
      c.sourceRGBBlendFactor = factor(p.blend.src);
      c.destinationRGBBlendFactor = factor(p.blend.dst);
      c.sourceAlphaBlendFactor = factor(p.blend.src_alpha);
      c.destinationAlphaBlendFactor = factor(p.blend.dst_alpha);
      c.rgbBlendOperation = p.blend.op == BlendOp::ReverseSubtract ? MTLBlendOperationReverseSubtract : MTLBlendOperationAdd;
      c.alphaBlendOperation = p.blend.op_alpha == BlendOp::ReverseSubtract ? MTLBlendOperationReverseSubtract : MTLBlendOperationAdd;
      c.writeMask = (p.blend.write_rgb ? (MTLColorWriteMaskRed | MTLColorWriteMaskGreen | MTLColorWriteMaskBlue) : 0) |
                    (p.blend.write_alpha ? MTLColorWriteMaskAlpha : 0);
      d.depthAttachmentPixelFormat = pixel_format(p.depth);
      d.rasterSampleCount = std::max(p.samples, 1u);
      NSError* err = nil;
      id<MTLRenderPipelineState> state = [device_ newRenderPipelineStateWithDescriptor:d error:&err];
      if (!state) {
        if (error) *error = err ? err.localizedDescription.UTF8String : "Metal pipeline creation failed";
        return nullptr;
      }
      return out<Pipeline>(state);
    }
  }
  void release(Pipeline pipeline) override {
    if (pipeline == pipeline_) pipeline_ = nullptr;
    drop(pipeline);
  }

  void begin_frame() override {
    if (command_buffer_) end_frame(false);
    dispatch_semaphore_wait(inflight_, DISPATCH_TIME_FOREVER);
    pool_ = objc_autoreleasePoolPush();
    slot_ = (slot_ + 1) % kSlots;
    ring_[slot_].current = 0;
    ring_[slot_].used = 0;
    command_buffer_ = [queue_ commandBuffer];
  }
  Span alloc(size_t bytes, size_t align) override {
    Ring& r = ring_[slot_];
    align = std::max<size_t>(align, 4);
    for (;;) {
      if (r.current < r.blocks.size()) {
        id<MTLBuffer> b = r.blocks[r.current];
        const size_t at = (r.used + align - 1) & ~(align - 1);
        if (at + bytes <= b.length) {
          r.used = at + bytes;
          return Span{static_cast<uint8_t*>(b.contents) + at, static_cast<BufferObject*>((__bridge void*)b), at};
        }
        ++r.current; r.used = 0;
        continue;
      }
      const size_t size = std::max(kRingBlock, (bytes + 0xFFFF) & ~size_t(0xFFFF));
      id<MTLBuffer> b = [device_ newBufferWithLength:size options:MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined];
      if (!b) { std::fprintf(stderr, "metal: out of memory for a %zu byte upload\n", bytes); std::abort(); }
      b.label = @"upload ring";
      r.blocks.push_back(b);
      r.used = 0;
    }
  }
  bool begin_pass(const PassDesc& p) override {
    end_pass();
    if (!command_buffer_) return false;
    MTLRenderPassDescriptor* d = [MTLRenderPassDescriptor renderPassDescriptor];
    id<MTLTexture> color = nil;
    if (p.drawable) {
      if (!drawable_) {
        if (!drawable_size(nullptr, nullptr)) return false;
        drawable_ = [layer_ nextDrawable];
        if (!drawable_) return false;
      }
      color = drawable_.texture;
    } else {
      color = in<id<MTLTexture>>(p.color);
    }
    if (color) {
      d.colorAttachments[0].texture = color;
      d.colorAttachments[0].loadAction = load_action(p.color_load);
      d.colorAttachments[0].clearColor = MTLClearColorMake(p.clear_color[0], p.clear_color[1], p.clear_color[2], p.clear_color[3]);
      if (p.resolve_color) {
        d.colorAttachments[0].resolveTexture = in<id<MTLTexture>>(p.resolve_color);
        d.colorAttachments[0].storeAction = MTLStoreActionStoreAndMultisampleResolve;
      } else {
        d.colorAttachments[0].storeAction = MTLStoreActionStore;
      }
    }
    if (p.depth) {
      d.depthAttachment.texture = in<id<MTLTexture>>(p.depth);
      d.depthAttachment.loadAction = load_action(p.depth_load);
      d.depthAttachment.clearDepth = p.clear_depth;
      if (p.resolve_depth) {
        d.depthAttachment.resolveTexture = in<id<MTLTexture>>(p.resolve_depth);
        d.depthAttachment.depthResolveFilter = MTLMultisampleDepthResolveFilterSample0;
        d.depthAttachment.storeAction = MTLStoreActionStoreAndMultisampleResolve;
      } else {
        d.depthAttachment.storeAction = MTLStoreActionStore;
      }
    }
    encoder_ = [command_buffer_ renderCommandEncoderWithDescriptor:d];
    if (!encoder_) return false;
    pass_descriptor_ = d;
    pass_w_ = int(color ? color.width : in<id<MTLTexture>>(p.depth).width);
    pass_h_ = int(color ? color.height : in<id<MTLTexture>>(p.depth).height);
    forget_state();
    return true;
  }
  void end_pass() override {
    if (encoder_) [encoder_ endEncoding];
    encoder_ = nil;
    pass_descriptor_ = nil;
  }
  bool in_pass() const override { return encoder_ != nil; }
  void end_frame(bool present) override {
    end_pass();
    if (!command_buffer_) return;
    if (present && drawable_) [command_buffer_ presentDrawable:drawable_];
    dispatch_semaphore_t done = inflight_;
    [command_buffer_ addCompletedHandler:^(id<MTLCommandBuffer> buffer) {
      if (buffer.status == MTLCommandBufferStatusError && buffer.error)
        std::fprintf(stderr, "metal: GPU error: %s\n", buffer.error.localizedDescription.UTF8String);
      dispatch_semaphore_signal(done);
    }];
    [command_buffer_ commit];
    command_buffer_ = nil;
    drawable_ = nil;
    if (pool_) { objc_autoreleasePoolPop(pool_); pool_ = nullptr; }
  }

  void set_pipeline(Pipeline pipeline) override {
    if (!encoder_ || pipeline == pipeline_ || !pipeline) return;
    pipeline_ = pipeline;
    [encoder_ setRenderPipelineState:in<id<MTLRenderPipelineState>>(pipeline)];
  }
  void set_depth_state(DepthState state) override {
    if (!encoder_ || state == depth_) return;
    depth_ = state;
    [encoder_ setDepthStencilState:in<id<MTLDepthStencilState>>(state)];
  }
  void set_viewport(float x, float y, float w, float h, float znear, float zfar) override {
    if (!encoder_) return;
    const std::array<float, 6> v{x, y, w, h, znear, zfar};
    if (v == viewport_) return;
    viewport_ = v;
    [encoder_ setViewport:(MTLViewport){x, y, w, h, znear, zfar}];
  }
  void set_scissor(int x, int y, int w, int h) override {
    if (!encoder_) return;
    const int l = std::clamp(x, 0, pass_w_), t = std::clamp(y, 0, pass_h_);
    const int r = std::clamp(x + w, 0, pass_w_), b = std::clamp(y + h, 0, pass_h_);
    scissor_empty_ = r <= l || b <= t;
    if (scissor_empty_) return;
    const std::array<int, 4> s{l, t, r - l, b - t};
    if (s == scissor_) return;
    scissor_ = s;
    [encoder_ setScissorRect:(MTLScissorRect){NSUInteger(l), NSUInteger(t), NSUInteger(r - l), NSUInteger(b - t)}];
  }
  void set_cull(Cull cull, bool clockwise_front) override {
    if (!encoder_) return;
    const int c = static_cast<int>(cull);
    if (c != cull_) {
      cull_ = c;
      [encoder_ setCullMode:cull == Cull::Front ? MTLCullModeFront : cull == Cull::Back ? MTLCullModeBack : MTLCullModeNone];
    }
    const int w = clockwise_front ? 1 : 0;
    if (w != winding_) {
      winding_ = w;
      [encoder_ setFrontFacingWinding:clockwise_front ? MTLWindingClockwise : MTLWindingCounterClockwise];
    }
  }
  void set_fill_lines(bool lines) override {
    if (!encoder_) return;
    const int f = lines ? 1 : 0;
    if (f == fill_) return;
    fill_ = f;
    [encoder_ setTriangleFillMode:lines ? MTLTriangleFillModeLines : MTLTriangleFillModeFill];
  }
  void set_vertex_buffer(const Span& span, int index) override {
    if (!encoder_ || !span.buffer) return;
    auto& bound = vertex_buffers_[index & 31];
    if (bound.buffer == span.buffer) {
      if (bound.offset != span.offset) [encoder_ setVertexBufferOffset:span.offset atIndex:index];
    } else {
      [encoder_ setVertexBuffer:(__bridge id<MTLBuffer>)(void*)span.buffer offset:span.offset atIndex:index];
    }
    bound = span;
  }
  void set_fragment_buffer(const Span& span, int index) override {
    if (!encoder_ || !span.buffer) return;
    auto& bound = fragment_buffers_[index & 31];
    if (bound.buffer == span.buffer) {
      if (bound.offset != span.offset) [encoder_ setFragmentBufferOffset:span.offset atIndex:index];
    } else {
      [encoder_ setFragmentBuffer:(__bridge id<MTLBuffer>)(void*)span.buffer offset:span.offset atIndex:index];
    }
    bound = span;
  }
  void set_vertex_bytes(const void* data, size_t bytes, int index) override {
    if (!encoder_) return;
    vertex_buffers_[index & 31] = Span{};
    [encoder_ setVertexBytes:data length:bytes atIndex:index];
  }
  void set_fragment_bytes(const void* data, size_t bytes, int index) override {
    if (!encoder_) return;
    fragment_buffers_[index & 31] = Span{};
    [encoder_ setFragmentBytes:data length:bytes atIndex:index];
  }
  void set_fragment_texture(Texture texture, int index) override {
    if (!encoder_ || index < 0 || index >= 16) return;
    if (textures_[index] == texture && textures_known_[index]) return;
    textures_[index] = texture; textures_known_[index] = true;
    [encoder_ setFragmentTexture:in<id<MTLTexture>>(texture) atIndex:index];
  }
  void set_fragment_sampler(Sampler sampler, int index) override {
    if (!encoder_ || index < 0 || index >= 16) return;
    if (samplers_[index] == sampler && samplers_known_[index]) return;
    samplers_[index] = sampler; samplers_known_[index] = true;
    [encoder_ setFragmentSamplerState:in<id<MTLSamplerState>>(sampler) atIndex:index];
  }
  void draw_indexed(Primitive primitive, uint32_t count, const Span& indices) override {
    if (!encoder_ || !count || !indices.buffer || scissor_empty_) return;
    [encoder_ drawIndexedPrimitives:primitive == Primitive::Lines ? MTLPrimitiveTypeLine : MTLPrimitiveTypeTriangle
                         indexCount:count
                          indexType:MTLIndexTypeUInt32
                        indexBuffer:(__bridge id<MTLBuffer>)(void*)indices.buffer
                  indexBufferOffset:indices.offset];
  }
  void draw(Primitive primitive, uint32_t first, uint32_t count) override {
    if (!encoder_ || !count || scissor_empty_) return;
    [encoder_ drawPrimitives:primitive == Primitive::Lines ? MTLPrimitiveTypeLine : MTLPrimitiveTypeTriangle
                 vertexStart:first vertexCount:count];
  }

  void copy_texture(Texture from, Texture to) override {
    if (!command_buffer_ || !from || !to) return;
    end_pass();
    id<MTLTexture> a = in<id<MTLTexture>>(from), b = in<id<MTLTexture>>(to);
    id<MTLBlitCommandEncoder> blit = [command_buffer_ blitCommandEncoder];
    [blit copyFromTexture:a sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake(std::min(a.width, b.width), std::min(a.height, b.height), 1)
                toTexture:b destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
    [blit endEncoding];
  }
  bool read_pixels(Texture texture, int x, int y, int w, int h, std::vector<uint8_t>& rgba) override {
    if (!command_buffer_ || w <= 0 || h <= 0) return false;
    end_pass();
    id<MTLTexture> t = texture ? in<id<MTLTexture>>(texture) : (drawable_ ? drawable_.texture : nil);
    if (!t) return false;
    x = std::clamp(x, 0, int(t.width)); y = std::clamp(y, 0, int(t.height));
    w = std::min(w, int(t.width) - x); h = std::min(h, int(t.height) - y);
    if (w <= 0 || h <= 0) return false;
    const size_t row = size_t(w) * 4;
    id<MTLBuffer> staging = [device_ newBufferWithLength:row * h options:MTLResourceStorageModeShared];
    id<MTLBlitCommandEncoder> blit = [command_buffer_ blitCommandEncoder];
    [blit copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(x, y, 0)
               sourceSize:MTLSizeMake(w, h, 1) toBuffer:staging destinationOffset:0
      destinationBytesPerRow:row destinationBytesPerImage:row * h];
    [blit endEncoding];
    [command_buffer_ commit];
    [command_buffer_ waitUntilCompleted];
    command_buffer_ = [queue_ commandBuffer];   // the rest of the frame
    rgba.resize(row * h);
    std::memcpy(rgba.data(), staging.contents, rgba.size());
    if (t.pixelFormat == MTLPixelFormatBGRA8Unorm)
      for (size_t i = 0; i < rgba.size(); i += 4) std::swap(rgba[i], rgba[i + 2]);
    return true;
  }

  void* native_command_buffer() override { return (__bridge void*)command_buffer_; }
  void* native_encoder() override { return (__bridge void*)encoder_; }
  void* native_pass_descriptor() override { return (__bridge void*)pass_descriptor_; }
  void forget_state() override {
    pipeline_ = nullptr; depth_ = nullptr;
    viewport_ = {-1, -1, -1, -1, -1, -1}; scissor_ = {-1, -1, -1, -1}; scissor_empty_ = false;
    cull_ = -1; winding_ = -1; fill_ = -1;
    for (auto& b : vertex_buffers_) b = Span{};
    for (auto& b : fragment_buffers_) b = Span{};
    for (int i = 0; i < 16; ++i) { textures_known_[i] = false; samplers_known_[i] = false; }
  }

 private:
  struct Ring { std::vector<id<MTLBuffer>> blocks; size_t current = 0, used = 0; };
  SDL_Window* window_;
  id<MTLDevice> device_;
  SDL_MetalView view_;
  CAMetalLayer* layer_;
  id<MTLCommandQueue> queue_;
  dispatch_semaphore_t inflight_;
  bool unified_ = false;
  Ring ring_[kSlots];
  int slot_ = 0;
  void* pool_ = nullptr;
  id<MTLCommandBuffer> command_buffer_;
  id<MTLRenderCommandEncoder> encoder_;
  MTLRenderPassDescriptor* pass_descriptor_;
  id<CAMetalDrawable> drawable_;
  int pass_w_ = 0, pass_h_ = 0;
  std::map<int, DepthState> depth_states_;
  // Cached encoder state.
  Pipeline pipeline_ = nullptr;
  DepthState depth_ = nullptr;
  std::array<float, 6> viewport_{};
  std::array<int, 4> scissor_{};
  bool scissor_empty_ = false;
  int cull_ = -1, winding_ = -1, fill_ = -1;
  Span vertex_buffers_[32], fragment_buffers_[32];
  Texture textures_[16]{};
  Sampler samplers_[16]{};
  bool textures_known_[16]{}, samplers_known_[16]{};
};

}  // namespace

std::unique_ptr<Device> Device::create(SDL_Window* window, bool vsync, std::string* error) {
  @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) {
      if (error) *error = "this Mac has no Metal device";
      return nullptr;
    }
    SDL_MetalView view = SDL_Metal_CreateView(window);
    if (!view) {
      if (error) *error = std::string("SDL could not make a Metal view: ") + SDL_GetError();
      return nullptr;
    }
    CAMetalLayer* layer = (__bridge CAMetalLayer*)SDL_Metal_GetLayer(view);
    if (!layer) {
      SDL_Metal_DestroyView(view);
      if (error) *error = "the window's Metal view has no layer";
      return nullptr;
    }
    return std::make_unique<MetalDevice>(window, device, view, layer, vsync);
  }
}

}  // namespace mtl
