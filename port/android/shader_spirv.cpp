// HLSL to SPIR-V for the Vulkan renderer on Android (runtime/gx/gx_vulkan.cpp compile_hlsl). Linux runs
// glslangValidator, a program an app cannot start; this is the same compiler as a library, with the
// same arguments (-D -V -e <entry> --target-env vulkan1.3 --auto-map-locations and the register
// shifts), and SPIRV-Tools' legalization, which glslangValidator applies to HLSL.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <glslang/SPIRV/GlslangToSpv.h>
#include <cstdint>
#include <mutex>
#include <regex>
#include <string>
#include <vector>

// Where glslang's HLSL reading differs from DXC's (which Windows and Linux use), the source is put the
// way glslang reads as DXC does: a push constant declared on a struct variable becomes a
// ConstantBuffer<struct> (glslang otherwise makes it an ordinary uniform buffer), and a shader that
// names its own bindings ([[vk::binding(n)]]) keeps them, as DXC keeps them under its register shifts.
static std::string for_glslang(const std::string& hlsl, bool* explicit_bindings) {
  static const std::regex push_constant(R"(\[\[vk::push_constant\]\]\s*struct\s+(\w+)\s*(\{[^}]*\})\s*(\w+)\s*;)");
  *explicit_bindings = hlsl.find("[[vk::binding(") != std::string::npos;
  return std::regex_replace(hlsl, push_constant, "struct $1 $2; [[vk::push_constant]] ConstantBuffer<$1> $3;");
}

std::vector<uint32_t> android_hlsl_spirv(const std::string& original, bool vertex, const char* entry, int texture_shift,
                                         int sampler_shift, bool invert_y, std::string* error) {
  static std::once_flag once;
  std::call_once(once, [] { glslang::InitializeProcess(); });
  bool explicit_bindings = false;
  const std::string hlsl = for_glslang(original, &explicit_bindings);
  if (explicit_bindings) texture_shift = sampler_shift = 0;
  const EShLanguage stage = vertex ? EShLangVertex : EShLangFragment;
  glslang::TShader shader(stage);
  const char* text = hlsl.c_str();
  const int length = (int)hlsl.size();
  shader.setStringsWithLengths(&text, &length, 1);
  shader.setEntryPoint(entry);
  shader.setSourceEntryPoint(entry);
  shader.setEnvInput(glslang::EShSourceHlsl, stage, glslang::EShClientVulkan, 100);
  shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_3);
  shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_6);
  shader.setShiftBinding(glslang::EResUbo, 0);
  shader.setShiftBinding(glslang::EResTexture, (unsigned)texture_shift);
  shader.setShiftBinding(glslang::EResSampler, (unsigned)sampler_shift);
  shader.setAutoMapLocations(true);
  if (invert_y) shader.setInvertY(true);
  const EShMessages messages = EShMessages(EShMsgSpvRules | EShMsgVulkanRules | EShMsgReadHlsl);
  if (!shader.parse(GetDefaultResources(), 100, false, messages)) {
    if (error) *error = std::string("glslang: ") + shader.getInfoLog();
    return {};
  }
  glslang::TProgram program;
  program.addShader(&shader);
  if (!program.link(messages) || !program.mapIO()) {
    if (error) *error = std::string("glslang link: ") + program.getInfoLog();
    return {};
  }
  std::vector<uint32_t> spirv;
  glslang::SpvOptions options;
  options.disableOptimizer = false;   // HLSL needs the legalization passes
  options.validate = false;
  spv::SpvBuildLogger logger;
  glslang::GlslangToSpv(*program.getIntermediate(stage), spirv, &logger, &options);
  if (spirv.empty() && error) *error = "glslang produced no SPIR-V: " + logger.getAllMessages();
  return spirv;
}
