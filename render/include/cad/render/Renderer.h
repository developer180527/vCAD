#pragma once
// A renderer, without saying which (ADR 0012).
//
// Backend.h is the seam a renderer is drawn THROUGH: three POD interfaces. This is the object that
// owns them, and the only way anything above `render/` gets one. No backend's type appears in this
// header, and none may be named above `render/`: a user whose machine cannot run one renderer is
// given another, and that only works if nothing was written against a particular one.

#include "cad/kernel/Result.h"
#include "cad/render/Backend.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cad::render {

struct RendererConfig {
    /// What to present to, or null for offscreen. TODAY this is what bgfx takes -- a CAMetalLayer
    /// on Apple, an HWND or X11 Window elsewhere -- and `app::Controller` makes the layer. ADR 0012
    /// step 5 turns it into the shell's view and moves that work into each renderer.
    void* nativeWindow = nullptr;
    void* nativeDisplay = nullptr;      ///< X11 only

    /// The scene's own target, in pixels.
    Viewport viewport;

    /// Ask the renderer for one graphics API by name ("metal", "vulkan", "d3d12"; "noop" validates
    /// and draws nothing). Empty lets it choose, which is what a shell wants.
    std::string api;

    /// Render into a framebuffer instead of a swap chain, and enable readback. Offscreen has no
    /// window to present to; on macOS the renderer still makes itself a layer to get a device.
    bool offscreen = false;

    /// Present to `nativeWindow` AND render the scene into a texture, for a shell that draws the
    /// viewport as one widget among others (shell_libgui). The swap chain is the whole window and
    /// sized by IRenderer::resizeSurface; `viewport` is the scene's own target, sized to the widget.
    bool sceneToTexture = false;
    /// The swap chain's size in pixels when `sceneToTexture` is set. Ignored otherwise.
    std::uint32_t surfaceWidth = 0;
    std::uint32_t surfaceHeight = 0;

    /// Where this renderer's compiled shaders are. Empty: the process default, setShaderDirectory.
    std::string shaderDirectory;

    /// Depth bias applied to edges, in NDC. Tuned by eye at 1e-4; too small z-fights, too large
    /// makes edges float visibly off silhouettes.
    float edgeDepthBias = 1.5e-4f;
    /// Ambient floor. CAD models are untextured, so this is most of what stops unlit faces reading
    /// as holes.
    float ambient = 0.35f;
};

/// What came up. For a person to read (About, the status bar, a bug report) and for the two facts
/// the layers above genuinely need.
struct RendererInfo {
    std::string id;                 ///< which renderer: "bgfx". Matches RendererFactory::id
    std::string api;                ///< which graphics API it is on: "Metal", "Vulkan", "Noop"
    /// NDC depth convention. Feed it to CameraController: getting it wrong depth-clips the whole
    /// scene and draws nothing, with no error.
    bool homogeneousDepth = false;
};

class IRenderer {
public:
    virtual ~IRenderer() = default;

    /// The three seam interfaces. Valid for as long as the renderer is.
    [[nodiscard]] virtual Backend handle() noexcept = 0;
    [[nodiscard]] virtual RendererInfo info() const = 0;

    /// The texture the scene rendered into, in sceneToTexture mode; TextureId::None otherwise.
    /// Changes when the viewport resizes, so read it every frame.
    [[nodiscard]] virtual TextureId sceneTexture() const = 0;

    /// Resizes the swap chain in sceneToTexture mode, where the window and the scene differ in size.
    virtual void resizeSurface(std::uint32_t width, std::uint32_t height) = 0;

    /// Reads the scene's colour buffer back as tightly packed RGBA8. The golden-image hook.
    virtual kernel::Result<std::vector<std::uint8_t>> captureFrame() = 0;
};

/// One renderer this build contains.
struct RendererFactory {
    const char* id;        ///< stable: what --renderer, CAD_RENDERER and a saved preference name
    const char* label;     ///< for a settings page
    int priority;          ///< higher is tried first when nothing was asked for
    /// Whether it is worth trying here, WITHOUT creating a device. Null means always.
    bool (*available)(std::string* why);
    /// Brings it up. A renderer that would draw nothing has FAILED and must say so: bgfx's Noop
    /// initialises happily and renders blank, and that has cost this project days, twice.
    kernel::Result<std::unique_ptr<IRenderer>> (*create)(const RendererConfig&);
};

/// Every renderer compiled into this build, highest priority first. A plain table rather than
/// self-registering statics: a static library's unreferenced objects are dropped by the linker, and
/// a renderer that is silently not there is the worst way to find that out.
[[nodiscard]] std::span<const RendererFactory> renderers();

/// One entry per renderer tried, in order, whether or not it worked.
struct RendererAttempt {
    std::string id;
    bool ok = false;
    std::string reason;   ///< why not, or empty
};

/// Brings up a renderer.
///
/// `preferred` names one (a command-line flag, a saved setting). Empty falls back to $CAD_RENDERER,
/// and failing that every renderer is tried in priority order and the first that comes up wins.
///
/// A renderer that was ASKED FOR and fails is an error; nothing else is tried. Someone who asked
/// for one renderer and silently got another has been lied to about what they are looking at.
kernel::Result<std::unique_ptr<IRenderer>> createRenderer(const RendererConfig&,
                                                          std::string_view preferred = {},
                                                          std::vector<RendererAttempt>* attempts = nullptr);

/// The process default for RendererConfig::shaderDirectory: the folder holding the compiled shaders,
/// which a shell sets once to the one beside its executable. $CAD_SHADER_DIR overrides both.
///
/// Process-wide because the shells that need it set it in main(), before any document or
/// controller exists to carry it.
void setShaderDirectory(std::string);
[[nodiscard]] const std::string& shaderDirectory();

}  // namespace cad::render
