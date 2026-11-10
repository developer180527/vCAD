#pragma once
// Private to cad_render: what the bgfx passes share. Not installed, not in include/.

#include <bgfx/bgfx.h>

#include "cad/render/Backend.h"

#include <string>

namespace cad::render::detail {

/// A compiled shader by stem ("vs_ui"), resolved the same way the scene's shaders are: see
/// shaderPath in BgfxBackend.cpp. Invalid handle when the file is missing.
bgfx::ShaderHandle loadShaderBinary(const std::string& name);

/// TextureId <-> bgfx. Offset by one because bgfx's index 0 is a real texture and TextureId's 0 is
/// None; the one place that knows, so the two directions cannot disagree.
[[nodiscard]] inline TextureId toTextureId(bgfx::TextureHandle h) noexcept {
    return bgfx::isValid(h) ? TextureId{std::uint64_t(h.idx) + 1} : TextureId::None;
}
[[nodiscard]] inline bgfx::TextureHandle toBgfxTexture(TextureId id) noexcept {
    const auto v = static_cast<std::uint64_t>(id);
    // Out of range is somebody else's id, or a number that was never an id. Invalid, not truncated.
    if (v == 0 || v > bgfx::kInvalidHandle) return BGFX_INVALID_HANDLE;
    return bgfx::TextureHandle{std::uint16_t(v - 1)};
}

}  // namespace cad::render::detail
