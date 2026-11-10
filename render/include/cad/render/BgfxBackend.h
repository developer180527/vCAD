#pragma once

// The real backend. Implements the three seam interfaces over bgfx.
//
// Three init modes:
//
//   * Windowed — a native window handle from the shell (Qt, GLFW, a CAMetalLayer on iPad).
//   * Offscreen — renders into a framebuffer and reads it back, for picking and golden images.
//   * Noop — bgfx validates every call and draws nothing. The CI path.
//
// IMPORTANT, learned the hard way: **offscreen still needs a window handle.** bgfx cannot
// create a Metal device on macOS without a CAMetalLayer, and with no window it silently selects
// `RendererType::Noop` — init succeeds, nothing rasterises, and the only clue is the renderer
// name. Offscreen means "draw into my framebuffer instead of the swap chain", NOT "no window".
// Use a hidden window; see spikes/bgfx_offscreen.
//
// Backbuffer sizing follows the WINDOW, not the offscreen flag: windowless init needs 0x0 (and
// only ever yields Noop), while a hidden window needs its REAL size even when rendering
// offscreen — Metal's render thread blocks forever acquiring a drawable from a zero-sized layer,
// which looks like a hang with no error at all.

#include "cad/kernel/Result.h"
#include "cad/render/Backend.h"
#include "cad/render/Renderer.h"

#include <memory>
#include <string>

namespace cad::render {

/// The bgfx renderer's configuration is the common one. The name is kept for the spikes, which use
/// this class directly; nothing above `render/` may (ADR 0012).
using BgfxConfig = RendererConfig;

/// Owns the bgfx context. One per process — bgfx is a singleton, which is a real constraint
/// rather than an implementation detail: two viewports share one backend and differ by view id.
///
/// Reached through render::createRenderer, as an IRenderer. The concrete methods below are for the
/// spikes and for the other bgfx passes inside `render/`.
class BgfxBackend final : public IRenderer {
public:
    BgfxBackend();
    ~BgfxBackend() override;
    BgfxBackend(const BgfxBackend&) = delete;
    BgfxBackend& operator=(const BgfxBackend&) = delete;

    /// Initialises bgfx and compiles nothing — shaders are loaded from the build's shader
    /// directory, so a missing shader is a clear error rather than a blank screen.
    kernel::Result<void> initialise(const BgfxConfig&);
    void shutdown();

    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] Backend handle() noexcept override;
    [[nodiscard]] RendererInfo info() const override;

    /// Which renderer bgfx actually chose. Worth surfacing: "why is it slow" is usually
    /// "it fell back to OpenGL", and "why is it blank" was "it fell back to Noop".
    [[nodiscard]] std::string rendererName() const;

    /// NDC depth convention of the chosen renderer. Feed this to CameraController — getting it
    /// wrong depth-clips everything and draws nothing at all.
    [[nodiscard]] bool homogeneousDepth() const;

    /// Whether the shader programs actually loaded. A viewport that initialises and draws
    /// nothing is the hardest thing to diagnose from a bug report, so make it queryable.
    [[nodiscard]] bool programsReady() const;

    /// Reads the colour buffer back into RGBA8. Offscreen only. This is the golden-image hook
    /// and the reason the offscreen mode exists.
    kernel::Result<std::vector<std::uint8_t>> captureFrame() override;

    /// The texture the scene rendered into, or TextureId::None when there is none (plain windowed
    /// mode). Valid until the viewport is resized: re-read it every frame.
    [[nodiscard]] TextureId sceneTexture() const override;

    /// Resizes the swap chain in `sceneToTexture` mode, where the window and the scene differ in
    /// size. The caller resizes the CAMetalLayer first (MetalSurface.h).
    void resizeSurface(std::uint32_t width, std::uint32_t height) override;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace cad::render
