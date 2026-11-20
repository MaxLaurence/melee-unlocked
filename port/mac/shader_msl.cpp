// HLSL to MSL for the Metal renderer: see shader_msl.h.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "shader_msl.h"
#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <glslang/SPIRV/GlslangToSpv.h>
#include <spirv_cross/spirv_msl.hpp>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>

namespace mac_shaders {
namespace {

void replace_all(std::string& text, const std::string& from, const std::string& to) {
  size_t at = 0;
  while ((at = text.find(from, at)) != std::string::npos) { text.replace(at, from.size(), to); at += to.size(); }
}

// The pixel shaders declare their eight textures and samplers as arrays with constant indices. One
// named resource per slot gives each its own Metal slot and lets unused ones drop out, the same
// rewrite the OpenGL translation makes.
std::string split_texture_arrays(std::string source) {
  std::string samplers, textures;
  for (unsigned i = 0; i < 8; ++i) {
    samplers += "SamplerState samp" + std::to_string(i) + " : register(s" + std::to_string(i) + ");\n";
    textures += "Texture2D Tex" + std::to_string(i) + " : register(t" + std::to_string(i) + ");\n";
  }
  replace_all(source, "SamplerState samp[8] : register(s0);", samplers);
  replace_all(source, "Texture2D Tex[8] : register(t0);", textures);
  for (unsigned i = 0; i < 8; ++i) {
    replace_all(source, "Tex[" + std::to_string(i) + "]", "Tex" + std::to_string(i));
    replace_all(source, "samp[" + std::to_string(i) + "]", "samp" + std::to_string(i));
  }
  return source;
}

bool hlsl_to_spirv(const std::string& hlsl, bool vertex, std::vector<uint32_t>& spirv, std::string* error) {
  static std::once_flag once;
  std::call_once(once, [] { glslang::InitializeProcess(); });
  const EShLanguage stage = vertex ? EShLangVertex : EShLangFragment;
  glslang::TShader shader(stage);
  const char* text = hlsl.c_str();
  shader.setStrings(&text, 1);
  shader.setEntryPoint("main");
  shader.setSourceEntryPoint("main");
  // As glslangValidator -D -V --auto-map-locations --auto-map-bindings --hlsl-iomap (the OpenGL path).
  shader.setEnvInput(glslang::EShSourceHlsl, stage, glslang::EShClientVulkan, 100);
  shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
  shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);
  shader.setHlslIoMapping(true);
  shader.setAutoMapBindings(true);
  shader.setAutoMapLocations(true);
  const EShMessages messages = EShMessages(EShMsgSpvRules | EShMsgVulkanRules | EShMsgReadHlsl | EShMsgHlslLegalization);
  if (!shader.parse(GetDefaultResources(), 100, false, messages)) {
    if (error) *error = std::string("glslang: ") + shader.getInfoLog();
    return false;
  }
  glslang::TProgram program;
  program.addShader(&shader);
  if (!program.link(messages) || !program.mapIO()) {
    if (error) *error = std::string("glslang link: ") + program.getInfoLog();
    return false;
  }
  glslang::SpvOptions options;
  options.disableOptimizer = true;
  spv::SpvBuildLogger logger;
  glslang::GlslangToSpv(*program.getIntermediate(stage), spirv, &logger, &options);
  if (spirv.empty()) {
    if (error) *error = "glslang produced no SPIR-V: " + logger.getAllMessages();
    return false;
  }
  return true;
}

int slot_of(const std::string& name, const char* prefix) {
  const size_t n = std::strlen(prefix);
  if (name.size() != n + 1 || name.compare(0, n, prefix) != 0) return -1;
  const char d = name[n];
  return d >= '0' && d <= '7' ? d - '0' : -1;
}

}  // namespace

bool translate(const std::string& hlsl, bool vertex, Msl& out, std::string* error) {
  std::vector<uint32_t> spirv;
  if (!hlsl_to_spirv(vertex ? hlsl : split_texture_arrays(hlsl), vertex, spirv, error)) return false;
  try {
    spirv_cross::CompilerMSL msl(std::move(spirv));
    auto options = msl.get_msl_options();
    options.platform = spirv_cross::CompilerMSL::Options::macOS;
    options.set_msl_version(2, 3, 0);
    options.pad_fragment_output_components = true;
    msl.set_msl_options(options);
    auto common = msl.get_common_options();
    // The GX shaders are written for Direct3D's clip space (z 0..1, y up) and pixel grid (top
    // left), which are Metal's too: nothing is flipped or remapped.
    common.vertex.flip_vert_y = false;
    common.vertex.fixup_clipspace = false;
    msl.set_common_options(common);
    const spv::ExecutionModel model = vertex ? spv::ExecutionModelVertex : spv::ExecutionModelFragment;
    const auto resources = msl.get_shader_resources();
    auto bind = [&](spirv_cross::ID id, uint32_t binding, int buffer, int texture, int sampler) {
      msl.set_decoration(id, spv::DecorationDescriptorSet, 0);
      msl.set_decoration(id, spv::DecorationBinding, binding);
      spirv_cross::MSLResourceBinding b;
      b.stage = model; b.desc_set = 0; b.binding = binding;
      if (buffer >= 0) b.msl_buffer = uint32_t(buffer);
      if (texture >= 0) b.msl_texture = uint32_t(texture);
      if (sampler >= 0) b.msl_sampler = uint32_t(sampler);
      msl.add_msl_resource_binding(b);
    };
    for (const auto& r : resources.uniform_buffers) {
      const std::string block = msl.get_name(r.base_type_id), name = r.name;
      if (block != "VSBlock" && block != "PSBlock" && name != "VSBlock" && name != "PSBlock") {
        if (error) *error = "unexpected constant buffer " + block + " " + name;
        return false;
      }
      bind(r.id, 0, kConstantsIndex, -1, -1);
    }
    for (const auto& r : resources.separate_images) {
      const int slot = slot_of(r.name, "Tex");
      if (slot < 0) { if (error) *error = "unexpected texture " + r.name; return false; }
      bind(r.id, 10 + slot, -1, slot, -1);
    }
    for (const auto& r : resources.separate_samplers) {
      const int slot = slot_of(r.name, "samp");
      if (slot < 0) { if (error) *error = "unexpected sampler " + r.name; return false; }
      bind(r.id, 20 + slot, -1, -1, slot);
    }
    if (!resources.sampled_images.empty() || !resources.storage_buffers.empty() || !resources.storage_images.empty()) {
      if (error) *error = "the shader uses resources the Metal renderer does not bind";
      return false;
    }
    out = Msl{};
    if (vertex)
      for (const auto& r : resources.stage_inputs)
        out.inputs.push_back({r.name, msl.get_decoration(r.id, spv::DecorationLocation)});
    out.source = msl.compile();
    const auto entries = msl.get_entry_points_and_stages();
    if (entries.empty()) { if (error) *error = "no entry point"; return false; }
    out.entry = msl.get_cleansed_entry_point_name(entries[0].name, entries[0].execution_model);
    return !out.source.empty();
  } catch (const std::exception& e) {
    if (error) *error = std::string("SPIRV-Cross: ") + e.what();
    return false;
  }
}

std::string serialize(const Msl& msl) {
  std::ostringstream o;
  o << "// melee-unlocked-msl entry=" << msl.entry << " inputs=";
  for (size_t i = 0; i < msl.inputs.size(); ++i) o << (i ? "," : "") << msl.inputs[i].name << ':' << msl.inputs[i].location;
  o << '\n' << msl.source;
  return o.str();
}

bool parse(const std::string& text, Msl& out) {
  const std::string tag = "// melee-unlocked-msl entry=";
  if (text.compare(0, tag.size(), tag) != 0) return false;
  const size_t eol = text.find('\n');
  if (eol == std::string::npos) return false;
  const std::string header = text.substr(tag.size(), eol - tag.size());
  const size_t space = header.find(" inputs=");
  if (space == std::string::npos) return false;
  out = Msl{};
  out.entry = header.substr(0, space);
  std::stringstream list(header.substr(space + 8));
  std::string item;
  while (std::getline(list, item, ',')) {
    const size_t colon = item.rfind(':');
    if (colon == std::string::npos) return false;
    out.inputs.push_back({item.substr(0, colon), uint32_t(std::strtoul(item.c_str() + colon + 1, nullptr, 10))});
  }
  out.source = text.substr(eol + 1);
  return !out.entry.empty() && !out.source.empty();
}

}  // namespace mac_shaders
