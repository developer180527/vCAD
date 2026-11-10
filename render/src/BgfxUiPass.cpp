#include "cad/render/UiPass.h"

#include "BgfxDetail.h"
#include "cad/render/BgfxBackend.h"

#include <bgfx/bgfx.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace cad::render {

using kernel::Error;
using kernel::ErrorCode;

namespace {

/// What render/shaders/ui/vs_ui.sc reads, in order, and where each lands. The names are libgui's
/// (libgui_vertex_attribute_name), because that is the vocabulary the layout arrives in; the check
/// in `create` is what makes a renamed or resized attribute a startup error rather than a frame of
/// garbage.
struct Expected {
    const char* name;
    std::uint32_t floats;
    bgfx::Attrib::Enum slot;
};
constexpr Expected kLayout[] = {
    {"pos", 2, bgfx::Attrib::Position},       {"local", 2, bgfx::Attrib::TexCoord0},
    {"uv", 2, bgfx::Attrib::TexCoord1},       {"color", 4, bgfx::Attrib::Color0},
    {"border_color", 4, bgfx::Attrib::Color1}, {"clip", 4, bgfx::Attrib::TexCoord2},
    {"params", 4, bgfx::Attrib::TexCoord3},   {"half_size", 2, bgfx::Attrib::TexCoord4},
    {"seg", 4, bgfx::Attrib::TexCoord5},
};

/// The UI draws after the scene's views (0 shaded, 1 pick, 2 blit) and one view per window, so a
/// torn-off panel's window is a view of its own. 64 windows is far past any real layout.
constexpr bgfx::ViewId kFirstUiView = 16;
constexpr std::uint32_t kMaxSurfaces = 64;

std::uint32_t rgba8(const float c[4]) {
    const auto u8 = [](float f) { return std::uint32_t(std::lround(std::clamp(f, 0.0f, 1.0f) * 255.0f)); };
    return (u8(c[0]) << 24) | (u8(c[1]) << 16) | (u8(c[2]) << 8) | u8(c[3]);
}

}  // namespace

struct UiPass::Impl {
    bgfx::VertexLayout layout;
    std::uint32_t stride = 0;
    bgfx::ProgramHandle program = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle uScreen = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle sTex = BGFX_INVALID_HANDLE;

    /// The glyph atlas, shared: libgui gives every window the same one (libgui_ui_new_sharing_fonts).
    bgfx::TextureHandle atlas = BGFX_INVALID_HANDLE;
    std::uint32_t atlasSize = 0;
    std::uint64_t atlasVersion = UINT64_MAX;

    struct Surface {
        bgfx::FrameBufferHandle fb = BGFX_INVALID_HANDLE;  ///< invalid for the backbuffer
        std::uint32_t w = 0, h = 0;
        bgfx::ViewId view = 0;
    };
    std::unordered_map<void*, Surface> surfaces;
    std::uint32_t nextView = 0;

    HostTextureFn hostTexture = nullptr;
    void* hostUser = nullptr;
    std::vector<bgfx::TextureHandle> images;  ///< createImage's, destroyed with the pass

    Surface* surfaceFor(void* key, std::uint32_t w, std::uint32_t h) {
        auto it = surfaces.find(key);
        if (it == surfaces.end()) {
            if (nextView >= kMaxSurfaces) return nullptr;
            it = surfaces.emplace(key, Surface{BGFX_INVALID_HANDLE, 0, 0, bgfx::ViewId(kFirstUiView + nextView++)})
                     .first;
        }
        Surface& s = it->second;
        // The backbuffer is sized by bgfx::reset, which the backend owns; a surface of our own is
        // a swap chain we recreate when its window resizes.
        if (key != nullptr && (s.w != w || s.h != h || !bgfx::isValid(s.fb))) {
            if (bgfx::isValid(s.fb)) bgfx::destroy(s.fb);
            s.fb = bgfx::createFrameBuffer(key, std::uint16_t(w), std::uint16_t(h));
        }
        s.w = w;
        s.h = h;
        return &s;
    }

    void uploadAtlas(const UiFrame& f) {
        if (f.atlas == nullptr || f.atlasSize == 0 || f.atlasVersion == atlasVersion) return;
        const bgfx::Memory* mem = bgfx::copy(f.atlas, f.atlasSize * f.atlasSize);
        if (bgfx::isValid(atlas) && atlasSize == f.atlasSize) {
            bgfx::updateTexture2D(atlas, 0, 0, 0, 0, std::uint16_t(f.atlasSize), std::uint16_t(f.atlasSize), mem);
        } else {
            if (bgfx::isValid(atlas)) bgfx::destroy(atlas);
            atlas = bgfx::createTexture2D(std::uint16_t(f.atlasSize), std::uint16_t(f.atlasSize), false, 1,
                                          bgfx::TextureFormat::R8, BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP,
                                          nullptr);
            bgfx::updateTexture2D(atlas, 0, 0, 0, 0, std::uint16_t(f.atlasSize), std::uint16_t(f.atlasSize), mem);
            atlasSize = f.atlasSize;
        }
        atlasVersion = f.atlasVersion;
    }
};

UiPass::UiPass(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

UiPass::~UiPass() {
    if (!impl_) return;
    for (auto& [key, s] : impl_->surfaces)
        if (bgfx::isValid(s.fb)) bgfx::destroy(s.fb);
    if (bgfx::isValid(impl_->atlas)) bgfx::destroy(impl_->atlas);
    for (auto t : impl_->images) bgfx::destroy(t);
    if (bgfx::isValid(impl_->program)) bgfx::destroy(impl_->program);
    if (bgfx::isValid(impl_->uScreen)) bgfx::destroy(impl_->uScreen);
    if (bgfx::isValid(impl_->sTex)) bgfx::destroy(impl_->sTex);
}

kernel::Result<std::unique_ptr<UiPass>> UiPass::create(BgfxBackend& backend, std::span<const UiAttribute> layout,
                                                       std::uint32_t stride) {
    if (!backend.ready()) return Error{ErrorCode::InvalidInput, "The renderer is not running."};

    const auto expectedCount = std::size(kLayout);
    if (layout.size() != expectedCount) {
        return Error{ErrorCode::Unsupported, "The UI's vertex format is not the one this renderer draws.",
                     "expected " + std::to_string(expectedCount) + " attributes, got " +
                         std::to_string(layout.size())};
    }
    auto impl = std::make_unique<Impl>();
    impl->layout.begin();
    for (std::size_t i = 0; i < expectedCount; ++i) {
        const auto& want = kLayout[i];
        const auto& got = layout[i];
        if (got.name != want.name || got.floats != want.floats || got.offset != impl->layout.getStride()) {
            impl->layout.end();
            return Error{ErrorCode::Unsupported, "The UI's vertex format is not the one this renderer draws.",
                         "attribute " + std::to_string(i) + ": expected '" + want.name + "' (" +
                             std::to_string(want.floats) + " floats at " +
                             std::to_string(impl->layout.getStride()) + "), got '" + got.name + "' (" +
                             std::to_string(got.floats) + " floats at " + std::to_string(got.offset) + ")"};
        }
        impl->layout.add(want.slot, std::uint8_t(want.floats), bgfx::AttribType::Float);
    }
    impl->layout.end();
    if (impl->layout.getStride() != stride) {
        return Error{ErrorCode::Unsupported, "The UI's vertex format is not the one this renderer draws.",
                     "stride " + std::to_string(stride) + ", attributes add up to " +
                         std::to_string(impl->layout.getStride())};
    }
    impl->stride = stride;

    bgfx::ShaderHandle vs = detail::loadShaderBinary("vs_ui");
    bgfx::ShaderHandle fs = detail::loadShaderBinary("fs_ui");
    if (!bgfx::isValid(vs) || !bgfx::isValid(fs)) {
        if (bgfx::isValid(vs)) bgfx::destroy(vs);
        if (bgfx::isValid(fs)) bgfx::destroy(fs);
        return Error{ErrorCode::Unsupported, "The interface's shaders could not be loaded.",
                     "vs_ui/fs_ui missing beside the scene's shaders"};
    }
    impl->program = bgfx::createProgram(vs, fs, true);
    impl->uScreen = bgfx::createUniform("u_screen", bgfx::UniformType::Vec4);
    impl->sTex = bgfx::createUniform("s_tex", bgfx::UniformType::Sampler);
    return std::unique_ptr<UiPass>(new UiPass(std::move(impl)));
}

void UiPass::setHostTextures(HostTextureFn fn, void* user) {
    impl_->hostTexture = fn;
    impl_->hostUser = user;
}

void UiPass::releaseSurface(void* surface) {
    auto it = impl_->surfaces.find(surface);
    if (it == impl_->surfaces.end()) return;
    if (bgfx::isValid(it->second.fb)) bgfx::destroy(it->second.fb);
    // The view id is not reused: a torn-off window is rare, and 64 is far more than a session opens.
    impl_->surfaces.erase(it);
}

void UiPass::submit(const UiFrame& f, void* surface, std::uint32_t pixelWidth, std::uint32_t pixelHeight) {
    Impl& im = *impl_;
    Impl::Surface* s = im.surfaceFor(surface, std::max(pixelWidth, 1u), std::max(pixelHeight, 1u));
    if (s == nullptr) return;

    bgfx::setViewFrameBuffer(s->view, s->fb);
    bgfx::setViewRect(s->view, 0, 0, std::uint16_t(s->w), std::uint16_t(s->h));
    bgfx::setViewClear(s->view, BGFX_CLEAR_COLOR, rgba8(f.clear));
    // Submission order IS paint order for a UI; the default sort would reorder by state.
    bgfx::setViewMode(s->view, bgfx::ViewMode::Sequential);
    bgfx::touch(s->view);

    im.uploadAtlas(f);
    if (f.vertexCount == 0 || f.indices.empty()) return;
    // A frame the transient pool cannot hold is not drawn at all rather than drawn in part. libgui
    // can cut its mesh to fit (libgui_set_mesh_limits); the shell sets that from these numbers.
    if (bgfx::getAvailTransientVertexBuffer(f.vertexCount, im.layout) < f.vertexCount ||
        bgfx::getAvailTransientIndexBuffer(std::uint32_t(f.indices.size()), true) < f.indices.size())
        return;

    bgfx::TransientVertexBuffer tvb;
    bgfx::TransientIndexBuffer tib;
    bgfx::allocTransientVertexBuffer(&tvb, f.vertexCount, im.layout);
    bgfx::allocTransientIndexBuffer(&tib, std::uint32_t(f.indices.size()), true);
    std::memcpy(tvb.data, f.vertices.data(), std::size_t(f.vertexCount) * im.stride);
    std::memcpy(tib.data, f.indices.data(), f.indices.size_bytes());

    const float screen[4] = {f.logicalWidth, f.logicalHeight, f.scale, 0.0f};
    for (const UiBatch& b : f.batches) {
        bgfx::TextureHandle t = im.atlas;
        if (b.host) {
            const TextureId id = im.hostTexture ? im.hostTexture(b.texture, im.hostUser) : TextureId::None;
            if (id == TextureId::None) continue;
            t = detail::toBgfxTexture(id);
        }
        if (!bgfx::isValid(t) || b.indexCount == 0) continue;
        bgfx::setUniform(im.uScreen, screen);
        bgfx::setTexture(0, im.sTex, t);
        bgfx::setVertexBuffer(0, &tvb);
        bgfx::setIndexBuffer(&tib, b.firstIndex, b.indexCount);
        bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A |
                       BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_INV_SRC_ALPHA));
        bgfx::submit(s->view, im.program);
    }
}

}  // namespace cad::render

namespace cad::render {

TextureId UiPass::createImage(std::span<const std::uint8_t> rgba, std::uint32_t w, std::uint32_t h) {
    if (w == 0 || h == 0 || w > 16384 || h > 16384 || rgba.size() != std::size_t(w) * h * 4) return TextureId::None;
    const bgfx::TextureHandle t = bgfx::createTexture2D(std::uint16_t(w), std::uint16_t(h), false, 1,
                                                        bgfx::TextureFormat::RGBA8, BGFX_SAMPLER_NONE,
                                                        bgfx::copy(rgba.data(), std::uint32_t(rgba.size())));
    if (!bgfx::isValid(t)) return TextureId::None;
    impl_->images.push_back(t);
    return detail::toTextureId(t);
}

}  // namespace cad::render
