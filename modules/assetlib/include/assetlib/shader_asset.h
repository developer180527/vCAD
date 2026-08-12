#pragma once
// ── Cooked shader (.cshader) — the declared interface plus the bytecode ─────
//
// A cooked shader is TWO things, and the second is the point:
//
//   1. Compiled bytecode for every (feature-set × backend profile) variant.
//   2. A DECLARED INTERFACE — the parameters and samplers a material may set,
//      with types and defaults.
//
// (2) is what makes materials data. Today `Material` is a fixed C++ struct
// (baseColor / normal / roughness / metallic), so "custom material" means
// "recompile the engine". Once a shader publishes its own parameter list, a
// material is just a shader reference plus values, and a project can define
// its own look without touching the engine — the stated goal of
// docs/plans/renderer-audit-and-plan.md Phase 5.
//
// THE ANTI-BLOAT RULE: features are a CLOSED list declared by the shader
// author, not an open node graph. Unreal's permutation explosion — and every
// piece of machinery built to survive it — follows from letting materials
// AUTHOR shaders. Here a material may only parameterize one. That keeps the
// full variant matrix small enough to simply cook all of it.
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace assetlib {

// ── Backend profiles ────────────────────────────────────────────────────────
// Which bytecode flavours a cooked shader carries. A dist needs whichever the
// shipping platform's bgfx renderer asks for; cooking is per-profile so a
// macOS box can produce a Vulkan build without producing a D3D one (bgfx's
// shaderc cannot compile D3D bytecode off Windows — shaderc_hlsl.cpp guards
// D3DCompiler on BX_PLATFORM_WINDOWS, and the DXIL loader's macOS fallback is
// literally "dxcompiler???").
enum ShaderProfileId : uint32_t {
    kProfileMetal = 0,   // macOS / iOS
    kProfileSpirv = 1,   // Vulkan — the cross-platform escape hatch
    kProfileDx11  = 2,   // s_5_0 DXBC   — WINDOWS HOST ONLY
    kProfileDx12  = 3,   // s_6_0 DXIL   — WINDOWS/LINUX HOST ONLY
    kProfileGlsl  = 4,   // desktop GL
    kProfileCount = 5,
};

const char* profileName(uint32_t profile);                  // "metal"
bool        profileFromName(const std::string& s, uint32_t& out);
const char* profileShadercProfile(uint32_t profile);        // shaderc --profile
const char* profileShadercPlatform(uint32_t profile);       // shaderc --platform
// False when this host's shaderc physically cannot emit that profile. Checked
// at cook time so the failure is "D3D shaders need a Windows host", not a
// shipped build that renders nothing.
bool        profileCookableOnThisHost(uint32_t profile);

// ── Declared interface ──────────────────────────────────────────────────────
enum ShaderParamType : uint32_t {
    kParamFloat = 0,
    kParamVec2  = 1,
    kParamVec3  = 2,
    kParamVec4  = 3,
    kParamColor = 4,   // vec4, but authored/edited as a colour
};
uint32_t    paramComponents(uint32_t type);   // floats consumed
const char* paramTypeName(uint32_t type);
bool        paramTypeFromName(const std::string& s, uint32_t& out);

// One material-settable value.
//
// Parameters do NOT each get their own GPU uniform. They pack into a shared
// vec4 uniform at a declared float offset — a material constant buffer, hand-
// declared. Two reasons: bgfx uniforms are set per-draw and a dozen tiny
// uniforms is a dozen setUniform calls per draw; and the existing shaders
// already pack this way (u_params.y is roughness), so the interface describes
// the shaders as they ARE rather than requiring a rewrite in the same change.
struct ShaderParam {
    std::string name;                 // material-facing: "roughness"
    uint32_t    type   = kParamFloat;
    std::string uniform;              // packs into: "u_params"
    uint32_t    offset = 0;           // float component within that uniform
    float       defaults[4] = { 0, 0, 0, 0 };
};

struct ShaderSampler {
    std::string name;                 // material-facing: "baseColor"
    std::string uniform;              // "s_baseColor"
    uint32_t    stage    = 0;         // bgfx texture stage
    std::string fallback;             // "white" | "flatNormal" | "" when unset
};

// One compiled (features × profile) combination. Offsets index ShaderAsset::blob.
struct ShaderVariant {
    uint32_t featureMask = 0;         // bit i = features[i] enabled
    uint32_t profile     = kProfileMetal;
    uint32_t vsOffset = 0, vsSize = 0;
    uint32_t fsOffset = 0, fsSize = 0;
};

// A hard cap, not a soft one. 32 features is 4 billion variants; anything
// approaching it means the closed-feature-list rule has been abandoned and the
// permutation explosion this format exists to prevent has already happened.
constexpr uint32_t kMaxShaderFeatures = 8;

struct ShaderAsset {
    std::string                name;
    std::vector<std::string>   features;   // index = bit position in featureMask
    std::vector<ShaderParam>   params;
    std::vector<ShaderSampler> samplers;
    std::vector<ShaderVariant> variants;
    std::vector<uint8_t>       blob;       // all bytecode, concatenated

    const ShaderVariant* find(uint32_t featureMask, uint32_t profile) const;
    const ShaderParam*   findParam(const std::string& n) const;
    const ShaderSampler* findSampler(const std::string& n) const;
    // Bit for a feature name; false when this shader doesn't declare it.
    bool featureBit(const std::string& n, uint32_t& outBit) const;
};

bool saveShader(const ShaderAsset& sh, const std::filesystem::path& outPath);
bool loadShader(ShaderAsset& out,      const std::filesystem::path& inPath);

} // namespace assetlib
