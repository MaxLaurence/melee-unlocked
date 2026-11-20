// The generated GX HLSL (gx_shader.cpp) as Metal Shading Language: glslang reads the HLSL into
// SPIR-V and SPIRV-Cross writes MSL, both linked into the program (no helper processes). The GX
// generator stays the one authoritative TEV/lighting implementation, as on every other renderer.
//
// Metal slots, the same for every translated shader:
//   vertex   VSBlock buffer(1), the vertex stream buffer(mtl::kVertexBufferIndex) through [[stage_in]]
//   fragment PSBlock buffer(1), Tex0..7 texture(0..7), samp0..7 sampler(0..7)
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace mac_shaders {

constexpr int kConstantsIndex = 1;

struct Input { std::string name; uint32_t location = 0; };
struct Msl {
  std::string source;        // MSL
  std::string entry;         // the function's name in it (SPIRV-Cross renames HLSL's main)
  std::vector<Input> inputs; // vertex functions: the stream attributes it declares, by HLSL name
};

// Any thread. False with the compiler's messages in `error`.
bool translate(const std::string& hlsl, bool vertex, Msl& out, std::string* error);
// The disk cache form: a header line with the entry and inputs, then the MSL.
std::string serialize(const Msl& msl);
bool parse(const std::string& text, Msl& out);

}  // namespace mac_shaders
