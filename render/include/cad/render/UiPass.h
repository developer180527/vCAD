#pragma once
// Draws a 2D UI -- quads that are already triangles -- over whatever the scene put on screen.
//
// Toolkit-free on purpose. The shell that uses this is built on libgui, but render/ may not know
// that: the pass takes vertices, indices and texture batches as plain memory, and the vertex layout
// is DESCRIBED by the caller and checked here, rather than copied from a toolkit's struct. A
// toolkit that changes its vertex fails `create` with a message naming the attribute, instead of
// drawing garbage. (libgui is pre-1.0 and will change it.)
//
// The shaders are render/shaders/ui/, a port of libgui's reference `ui.wgsl`: every value the
// fragment stage needs arrives interpolated in the vertex, so the vertex stage only maps logical
// pixels to clip space. Output is premultiplied alpha.

#include "cad/kernel/Result.h"
#include "cad/render/Backend.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace cad::render {

class BgfxBackend;

/// One vertex attribute as the toolkit lays it out: a name and a count of 32-bit floats, in order.
/// The pass knows which names its shader reads and refuses a layout that does not match.
struct UiAttribute {
    std::string name;
    std::uint32_t floats = 0;
    std::uint32_t offset = 0;   ///< bytes from the start of the vertex
};

/// A run of indices drawn with one texture.
struct UiBatch {
    /// false: the glyph atlas. true: a texture of the host's, looked up with `hostTexture`.
    bool host = false;
    std::uint64_t texture = 0;
    std::uint32_t firstIndex = 0;
    std::uint32_t indexCount = 0;
};

/// One window's UI for one frame. Everything is borrowed for the duration of `submit`.
struct UiFrame {
    std::span<const std::byte> vertices;       ///< `vertexCount` vertices of the create-time stride
    std::uint32_t vertexCount = 0;
    std::span<const std::uint32_t> indices;
    std::span<const UiBatch> batches;
    float logicalWidth = 0, logicalHeight = 0;  ///< the UI's coordinate space
    float scale = 1;                            ///< physical pixels per logical pixel
    float clear[4]{0, 0, 0, 1};                 ///< straight (not premultiplied) RGBA

    /// The glyph atlas: `atlasSize` x `atlasSize`, one byte of coverage per texel. Re-uploaded only
    /// when `atlasVersion` changes.
    const std::uint8_t* atlas = nullptr;
    std::uint32_t atlasSize = 0;
    std::uint64_t atlasVersion = 0;
};

/// Needs a running BgfxBackend: bgfx is a process singleton and the backend owns it.
class UiPass {
public:
    /// Fails, naming the problem, when `layout` is not what the UI shader reads or the shaders did
    /// not load.
    static kernel::Result<std::unique_ptr<UiPass>> create(BgfxBackend&, std::span<const UiAttribute> layout,
                                                          std::uint32_t stride);
    ~UiPass();
    UiPass(const UiPass&) = delete;
    UiPass& operator=(const UiPass&) = delete;

    /// Queues the UI for the next frame into `surface`'s swap chain, or into the backbuffer when
    /// `surface` is null. `surface` is a CAMetalLayer on macOS (see MetalSurface.h); the pass keeps
    /// one swap chain per surface and resizes it to `pixelWidth` x `pixelHeight`.
    ///
    /// Presenting is BgfxBackend's frame, not this: nothing here calls bgfx::frame().
    void submit(const UiFrame&, void* surface, std::uint32_t pixelWidth, std::uint32_t pixelHeight);

    /// Forget a surface's swap chain, before the window that owns the layer goes away.
    void releaseSurface(void* surface);

    /// Batches the UI marked `host` are drawn with this: return the texture to draw, one this
    /// renderer issued (the scene's, or createImage's), or TextureId::None to skip the batch.
    /// Unset: host batches are skipped.
    using HostTextureFn = TextureId (*)(std::uint64_t texture, void* user);
    void setHostTextures(HostTextureFn, void* user);

    /// Uploads an RGBA8 image the pass owns until it is destroyed, for a host texture such as an
    /// icon atlas. PREMULTIPLY it first and draw it with libgui's premultiplied image mode: bgfx
    /// filters in hardware, and straight alpha filtered that way haloes every edge. Returns the
    /// texture, for a HostTextureFn to hand back, or TextureId::None if the size is wrong.
    TextureId createImage(std::span<const std::uint8_t> rgba, std::uint32_t width, std::uint32_t height);

    struct Impl;

private:
    explicit UiPass(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl_;
};

}  // namespace cad::render
