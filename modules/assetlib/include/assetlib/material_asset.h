#pragma once
// ── Cooked material (.cmat) — a shader reference plus resolved values ───────
//
// This is what replaces the fixed `Material` struct. Today material.h hardcodes
// baseColorTexture / normalMapTexture / baseColorFactor / roughness / metallic,
// so a project wanting anything else must recompile the engine. A cooked
// material names a shader and carries values for the parameters THAT shader
// declared — so the set of material properties is data, not C++.
//
// PARAMETER NAMES ARE RESOLVED AT COOK TIME. The cooker reads the .shader's
// declared interface, checks every name and type, fills defaults, and emits
// finished float blocks. The runtime therefore does no name lookup and no
// validation: it uploads bytes. A misspelled parameter is a failed cook, not a
// silent no-op at 60 Hz.
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace assetlib {

// One shader uniform, fully populated. `values` is vec4-aligned (size % 4 == 0)
// because that is the granularity bgfx::setUniform works in.
//
// The block is COMPLETE, never sparse: parameters the material didn't override
// hold the shader's declared defaults. A partial block would leave whatever the
// previous draw wrote in the gaps — the classic "material looks right alone,
// wrong after another object draws" bug.
struct MaterialUniform {
    std::string        name;      // "u_params"
    std::vector<float> values;
};

struct MaterialTexture {
    std::string uniform;    // "s_baseColor"
    uint32_t    stage = 0;
    std::string path;       // project-relative SOURCE path, "" = use fallback
    std::string fallback;   // "white" | "flatNormal" — what to bind when unset
    // v3: the COOKED texture, relative to the cache root
    // ("textures/<uuid>.cooked"). Empty in a dev project, where `path` resolves
    // through the registry; filled by engine_build when packaging, because a
    // shipped dist has NO registry and a source path is unresolvable there.
    // Without it every textured material in a dist bound its white fallback —
    // the same failure meshes had before their sibling .ctex files shipped.
    //
    // The packager fills this rather than the cooker on purpose: the cooker can
    // run in a WORKER PROCESS, which receives only source/output/uuid on argv,
    // so it has no registry to resolve a path against. engine_build runs on the
    // dev machine with the cache in front of it.
    std::string cooked;
};

struct MaterialAsset {
    std::string name;
    // The shader's DECLARED NAME ("standard"). This is what the runtime
    // resolves by: a dist has no registry, so ShaderLibrary indexes cooked
    // shaders by the name each one carries inside itself. Storing only the
    // authored path would force the runtime to re-derive this from a filename.
    std::string shaderName;
    // Project-relative path as authored. Kept for diagnostics and tooling —
    // "which file do I edit?" — never for resolution.
    std::string shaderPath;
    uint32_t    featureMask = 0;    // which shader features this variant needs

    std::vector<MaterialUniform> uniforms;
    std::vector<MaterialTexture> textures;

    bool doubleSided = false;

    const MaterialUniform* findUniform(const std::string& n) const;
    const MaterialTexture* findTexture(const std::string& uniform) const;
};

bool saveMaterial(const MaterialAsset& m, const std::filesystem::path& outPath);
bool loadMaterial(MaterialAsset& out,     const std::filesystem::path& inPath);

} // namespace assetlib
