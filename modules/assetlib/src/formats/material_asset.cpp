#include "assetlib/material_asset.h"

#include <cstdio>

namespace assetlib {

namespace {

constexpr uint32_t kMagic   = 0x54414D43;   // 'CMAT'
constexpr uint32_t kVersion = 3;   // v3: MaterialTexture::cooked

void wr(FILE* f, const void* p, size_t n) { std::fwrite(p, 1, n, f); }
void wrU32(FILE* f, uint32_t v)           { wr(f, &v, 4); }
void wrStr(FILE* f, const std::string& s) {
    wrU32(f, (uint32_t)s.size());
    if (!s.empty()) wr(f, s.data(), s.size());
}

bool rd(FILE* f, void* p, size_t n) { return std::fread(p, 1, n, f) == n; }
bool rdU32(FILE* f, uint32_t& v)    { return rd(f, &v, 4); }
bool rdStr(FILE* f, std::string& s) {
    uint32_t n = 0;
    if (!rdU32(f, n)) return false;
    // Cooked assets travel through a SHARED DDC, so "another machine wrote
    // this" is the threat model — a corrupt length must not allocate a gigabyte.
    if (n > (1u << 20)) return false;
    s.assign(n, '\0');
    return n == 0 || rd(f, s.data(), n);
}

} // namespace

const MaterialUniform* MaterialAsset::findUniform(const std::string& n) const {
    for (const auto& u : uniforms) if (u.name == n) return &u;
    return nullptr;
}
const MaterialTexture* MaterialAsset::findTexture(const std::string& uniform) const {
    for (const auto& t : textures) if (t.uniform == uniform) return &t;
    return nullptr;
}

bool saveMaterial(const MaterialAsset& m, const std::filesystem::path& outPath) {
    FILE* f = std::fopen(outPath.string().c_str(), "wb");
    if (!f) return false;

    wrU32(f, kMagic);
    wrU32(f, kVersion);
    wrStr(f, m.name);
    wrStr(f, m.shaderName);
    wrStr(f, m.shaderPath);
    wrU32(f, m.featureMask);
    wrU32(f, m.doubleSided ? 1u : 0u);

    wrU32(f, (uint32_t)m.uniforms.size());
    for (const auto& u : m.uniforms) {
        wrStr(f, u.name);
        wrU32(f, (uint32_t)u.values.size());
        if (!u.values.empty())
            wr(f, u.values.data(), u.values.size() * sizeof(float));
    }

    wrU32(f, (uint32_t)m.textures.size());
    for (const auto& t : m.textures) {
        wrStr(f, t.uniform);
        wrU32(f, t.stage);
        wrStr(f, t.path);
        wrStr(f, t.fallback);
        wrStr(f, t.cooked);
    }

    const bool ok = std::ferror(f) == 0;
    std::fclose(f);
    return ok;
}

bool loadMaterial(MaterialAsset& out, const std::filesystem::path& inPath) {
    FILE* f = std::fopen(inPath.string().c_str(), "rb");
    if (!f) return false;
    auto fail = [&] { std::fclose(f); out = {}; return false; };

    uint32_t magic = 0, version = 0, flag = 0, n = 0;
    if (!rdU32(f, magic) || magic != kMagic)       return fail();
    if (!rdU32(f, version) || version != kVersion) return fail();
    if (!rdStr(f, out.name))       return fail();
    if (!rdStr(f, out.shaderName)) return fail();
    if (!rdStr(f, out.shaderPath)) return fail();
    if (!rdU32(f, out.featureMask)) return fail();
    if (!rdU32(f, flag))            return fail();
    out.doubleSided = flag != 0;

    if (!rdU32(f, n) || n > 256) return fail();
    out.uniforms.resize(n);
    for (auto& u : out.uniforms) {
        if (!rdStr(f, u.name)) return fail();
        uint32_t count = 0;
        if (!rdU32(f, count) || count > (1u << 16)) return fail();
        // Uniform data is uploaded as vec4 registers; a block that isn't a
        // multiple of 4 would make the register count ambiguous and read past
        // the vector on upload.
        if (count % 4 != 0) return fail();
        u.values.resize(count);
        if (count && !rd(f, u.values.data(), count * sizeof(float))) return fail();
    }

    if (!rdU32(f, n) || n > 256) return fail();
    out.textures.resize(n);
    for (auto& t : out.textures) {
        if (!rdStr(f, t.uniform) || !rdU32(f, t.stage) || !rdStr(f, t.path)
            || !rdStr(f, t.fallback) || !rdStr(f, t.cooked))
            return fail();
    }

    std::fclose(f);
    return true;
}

} // namespace assetlib
