// ── AssetType names and extension mapping ────────────────────────────────────
// The single place that decides "what kind of asset is this file". Extracted
// because it is pure, has no SQLite in it at all, and is what you edit when
// adding a new asset format — a task that should not require opening the
// database layer.
#include "assetlib/asset_registry.h"

namespace assetlib {

// ── Names ─────────────────────────────────────────────────────────────────────
std::string assetTypeName(AssetType t) {
    switch (t) {
        case AssetType::Mesh:     return "mesh";
        case AssetType::Texture:  return "texture";
        case AssetType::Material: return "material";
        case AssetType::Scene:    return "scene";
        case AssetType::Prefab:   return "prefab";
        case AssetType::Shader:   return "shader";
        case AssetType::Audio:    return "audio";
        default:                  return "unknown";
    }
}
std::string assetStateName(AssetState s) {
    switch (s) {
        case AssetState::Registered: return "registered";
        case AssetState::Importing:  return "importing";
        case AssetState::Ready:      return "ready";
        case AssetState::Stale:      return "stale";
        case AssetState::Missing:    return "missing";
        case AssetState::Failed:     return "failed";
        case AssetState::Dirty:      return "dirty";
        default:                     return "unknown";
    }
}
AssetType assetTypeFromExtension(const std::string& ext) {
    if (ext==".fbx"||ext==".obj"||ext==".glb"||ext==".gltf"||
        ext==".dae"||ext==".ply"||ext==".stl") return AssetType::Mesh;
    if (ext==".png"||ext==".jpg"||ext==".jpeg"||ext==".tga"||
        ext==".bmp"||ext==".hdr"||ext==".exr") return AssetType::Texture;
    // `.material` is the cookable unit; `.mat` predates it and has no cooker.
    if (ext==".material"||ext==".mat")           return AssetType::Material;
    if (ext==".scene")                          return AssetType::Scene;
    if (ext==".prefab")                         return AssetType::Prefab;
    // `.shader` is the COOKABLE unit — a manifest naming its .sc sources plus
    // the material interface they publish. The raw stage sources are inputs to
    // it, registered as assets but handled by no cooker of their own.
    if (ext==".shader")                         return AssetType::Shader;
    if (ext==".glsl"||ext==".sc"||ext==".hlsl") return AssetType::Shader;
    if (ext==".wav"||ext==".ogg"||ext==".mp3")  return AssetType::Audio;
    return AssetType::Unknown;
}

} // namespace assetlib
