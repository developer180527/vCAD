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
// Also: in offscreen mode the BACKBUFFER resolution must be 0x0, or bgfx refuses to init with
// "resolution of non-existing backbuffer can't be larger than 0x0". Our real resolution lives on
// the framebuffer we create, not on the swap chain that does not exist.

#include "cad/kernel/Result.h"
#include "cad/render/Backend.h"

#include <memory>
#include <string>

namespace cad::render {

struct BgfxConfig {
    /// Native window handle (NSWindow*, HWND, X11 Window). Null for offscreen.
    void* nativeWindow = nullptr;
    void* nativeDisplay = nullptr;      ///< X11 only
    Viewport viewport;

    /// Force a specific renderer. Empty means "let bgfx choose", which is Metal on macOS,
    /// D3D12 on Windows, Vulkan on Linux. "noop" is the CI path.
    std::string rendererName;

    /// Render into a framebuffer instead of the swap chain, and enable readback.
    ///
    /// Does NOT mean "no window" — see the note at the top of this file. On macOS a window is
    /// still required to get a Metal device at all.
    bool offscreen = false;

    /// Depth bias applied to edges, in NDC. Tuned by eye at 1e-4; too small z-fights, too
    /// large makes edges float visibly off silhouettes.
    float edgeDepthBias = 1.5e-4f;

    /// Ambient floor. CAD models are untextured, so this is most of what stops unlit faces
    /// reading as holes.
    float ambient = 0.35f;
};

/// Owns the bgfx context. One per process — bgfx is a singleton, which is a real constraint
/// rather than an implementation detail: two viewports share one backend and differ by view id.
class BgfxBackend {
public:
    BgfxBackend();
    ~BgfxBackend();
    BgfxBackend(const BgfxBackend&) = delete;
    BgfxBackend& operator=(const BgfxBackend&) = delete;

    /// Initialises bgfx and compiles nothing — shaders are loaded from the build's shader
    /// directory, so a missing shader is a clear error rather than a blank screen.
    kernel::Result<void> initialise(const BgfxConfig&);
    void shutdown();

    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] Backend handle() noexcept;

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
    kernel::Result<std::vector<std::uint8_t>> captureFrame();

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace cad::render
