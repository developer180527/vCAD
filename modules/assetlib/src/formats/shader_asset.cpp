#include "assetlib/shader_asset.h"

#include <cstdio>
#include <cstring>

namespace assetlib {

namespace {

struct ProfileInfo { const char* name; const char* shadercProfile; const char* platform; };
const ProfileInfo kProfiles[kProfileCount] = {
    { "metal", "metal", "osx"     },
    { "spirv", "spirv", "linux"   },   // bgfx cooks Vulkan SPIR-V under linux
    { "dx11",  "s_5_0", "windows" },
    { "dx12",  "s_6_0", "windows" },
    { "glsl",  "150",   "linux"   },
};

constexpr uint32_t kMagic   = 0x44485343;   // 'CSHD'
constexpr uint32_t kVersion = 1;

// ── binary helpers ──────────────────────────────────────────────────────────
void wr(FILE* f, const void* p, size_t n)   { std::fwrite(p, 1, n, f); }
void wrU32(FILE* f, uint32_t v)             { wr(f, &v, 4); }
void wrStr(FILE* f, const std::string& s) {
    wrU32(f, (uint32_t)s.size());
    if (!s.empty()) wr(f, s.data(), s.size());
}

bool rd(FILE* f, void* p, size_t n)         { return std::fread(p, 1, n, f) == n; }
bool rdU32(FILE* f, uint32_t& v)            { return rd(f, &v, 4); }
bool rdStr(FILE* f, std::string& s) {
    uint32_t n = 0;
    if (!rdU32(f, n)) return false;
    // A corrupt or hostile length must not turn into a gigabyte allocation.
    // Cooked assets are trusted-ish, but they also travel through a SHARED
    // DDC, so "someone else's machine wrote this" is a real threat model.
    if (n > (1u << 20)) return false;
    s.assign(n, '\0');
    return n == 0 || rd(f, s.data(), n);
}

} // namespace

const char* profileName(uint32_t p) {
    return p < kProfileCount ? kProfiles[p].name : "?";
}
const char* profileShadercProfile(uint32_t p) {
    return p < kProfileCount ? kProfiles[p].shadercProfile : "";
}
const char* profileShadercPlatform(uint32_t p) {
    return p < kProfileCount ? kProfiles[p].platform : "";
}
bool profileFromName(const std::string& s, uint32_t& out) {
    for (uint32_t i = 0; i < kProfileCount; ++i)
        if (s == kProfiles[i].name) { out = i; return true; }
    return false;
}

bool profileCookableOnThisHost(uint32_t p) {
    // Mirrors bgfx's own guards. Getting this wrong does not fail loudly at
    // cook time — it produces a package that renders nothing on the target,
    // which is exactly the class of bug that reaches players.
#if defined(_WIN32)
    (void)p; return p < kProfileCount;            // Windows hosts do everything
#elif defined(__linux__)
    return p != kProfileDx11;                     // d3d4linux covers DXIL, not DXBC
#else
    return p == kProfileMetal || p == kProfileSpirv || p == kProfileGlsl;
#endif
}

uint32_t paramComponents(uint32_t type) {
    switch (type) {
        case kParamFloat: return 1;
        case kParamVec2:  return 2;
        case kParamVec3:  return 3;
        case kParamVec4:
        case kParamColor: return 4;
        default:          return 0;
    }
}
const char* paramTypeName(uint32_t type) {
    switch (type) {
        case kParamFloat: return "float";
        case kParamVec2:  return "vec2";
        case kParamVec3:  return "vec3";
        case kParamVec4:  return "vec4";
        case kParamColor: return "color";
        default:          return "?";
    }
}
bool paramTypeFromName(const std::string& s, uint32_t& out) {
    if (s == "float") { out = kParamFloat; return true; }
    if (s == "vec2")  { out = kParamVec2;  return true; }
    if (s == "vec3")  { out = kParamVec3;  return true; }
    if (s == "vec4")  { out = kParamVec4;  return true; }
    if (s == "color") { out = kParamColor; return true; }
    return false;
}

const ShaderVariant* ShaderAsset::find(uint32_t mask, uint32_t profile) const {
    for (const auto& v : variants)
        if (v.featureMask == mask && v.profile == profile) return &v;
    return nullptr;
}
const ShaderParam* ShaderAsset::findParam(const std::string& n) const {
    for (const auto& p : params) if (p.name == n) return &p;
    return nullptr;
}
const ShaderSampler* ShaderAsset::findSampler(const std::string& n) const {
    for (const auto& s : samplers) if (s.name == n) return &s;
    return nullptr;
}
bool ShaderAsset::featureBit(const std::string& n, uint32_t& outBit) const {
    for (uint32_t i = 0; i < (uint32_t)features.size(); ++i)
        if (features[i] == n) { outBit = 1u << i; return true; }
    return false;
}

bool saveShader(const ShaderAsset& sh, const std::filesystem::path& outPath) {
    FILE* f = std::fopen(outPath.string().c_str(), "wb");
    if (!f) return false;

    wrU32(f, kMagic);
    wrU32(f, kVersion);
    wrStr(f, sh.name);

    wrU32(f, (uint32_t)sh.features.size());
    for (const auto& s : sh.features) wrStr(f, s);

    wrU32(f, (uint32_t)sh.params.size());
    for (const auto& p : sh.params) {
        wrStr(f, p.name);
        wrU32(f, p.type);
        wrStr(f, p.uniform);
        wrU32(f, p.offset);
        wr(f, p.defaults, sizeof(p.defaults));
    }

    wrU32(f, (uint32_t)sh.samplers.size());
    for (const auto& s : sh.samplers) {
        wrStr(f, s.name);
        wrStr(f, s.uniform);
        wrU32(f, s.stage);
        wrStr(f, s.fallback);
    }

    wrU32(f, (uint32_t)sh.variants.size());
    for (const auto& v : sh.variants) {
        wrU32(f, v.featureMask); wrU32(f, v.profile);
        wrU32(f, v.vsOffset);    wrU32(f, v.vsSize);
        wrU32(f, v.fsOffset);    wrU32(f, v.fsSize);
    }

    wrU32(f, (uint32_t)sh.blob.size());
    if (!sh.blob.empty()) wr(f, sh.blob.data(), sh.blob.size());

    const bool ok = std::ferror(f) == 0;
    std::fclose(f);
    return ok;
}

bool loadShader(ShaderAsset& out, const std::filesystem::path& inPath) {
    FILE* f = std::fopen(inPath.string().c_str(), "rb");
    if (!f) return false;
    auto fail = [&]{ std::fclose(f); return false; };

    uint32_t magic = 0, version = 0;
    if (!rdU32(f, magic) || magic != kMagic)   return fail();
    if (!rdU32(f, version) || version != kVersion) return fail();
    if (!rdStr(f, out.name)) return fail();

    uint32_t n = 0;
    if (!rdU32(f, n) || n > kMaxShaderFeatures) return fail();
    out.features.resize(n);
    for (auto& s : out.features) if (!rdStr(f, s)) return fail();

    if (!rdU32(f, n) || n > 4096) return fail();
    out.params.resize(n);
    for (auto& p : out.params) {
        if (!rdStr(f, p.name) || !rdU32(f, p.type) || !rdStr(f, p.uniform)
            || !rdU32(f, p.offset) || !rd(f, p.defaults, sizeof(p.defaults)))
            return fail();
    }

    if (!rdU32(f, n) || n > 256) return fail();
    out.samplers.resize(n);
    for (auto& s : out.samplers) {
        if (!rdStr(f, s.name) || !rdStr(f, s.uniform) || !rdU32(f, s.stage)
            || !rdStr(f, s.fallback))
            return fail();
    }

    if (!rdU32(f, n) || n > (kProfileCount << kMaxShaderFeatures)) return fail();
    out.variants.resize(n);
    for (auto& v : out.variants) {
        if (!rdU32(f, v.featureMask) || !rdU32(f, v.profile)
            || !rdU32(f, v.vsOffset) || !rdU32(f, v.vsSize)
            || !rdU32(f, v.fsOffset) || !rdU32(f, v.fsSize))
            return fail();
    }

    uint32_t blobSize = 0;
    if (!rdU32(f, blobSize)) return fail();
    out.blob.resize(blobSize);
    if (blobSize && !rd(f, out.blob.data(), blobSize)) return fail();
    std::fclose(f);

    // Every variant's slices must lie inside the blob. Without this a
    // truncated or tampered .cshader hands bgfx a pointer past the end of the
    // buffer — and it arrives as a GPU-driver crash with no provenance.
    for (const auto& v : out.variants) {
        const uint64_t vsEnd = (uint64_t)v.vsOffset + v.vsSize;
        const uint64_t fsEnd = (uint64_t)v.fsOffset + v.fsSize;
        if (vsEnd > blobSize || fsEnd > blobSize) { out = {}; return false; }
    }
    return true;
}

} // namespace assetlib
