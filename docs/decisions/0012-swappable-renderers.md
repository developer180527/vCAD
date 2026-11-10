# 0012 — Renderers are swappable, and chosen at start-up

Status: proposed (11 Nov 2026). Nothing here is implemented; `render/` is bgfx only.
Consumers: `render`, `app::Controller`, and all three shells (`shell_qt`, `shell_libgui`, `shell_ios`). Not `abi`: the session keeps its `NullBackend` and is unaffected.

On acceptance this supersedes two earlier statements, and each gets a line pointing here:

- [ADR 0007](0007-renderer-seam.md), first amendment: "the engine-swap requirement is withdrawn". A
  swap requirement is back, for a different reason (below), and it is a harder one.
- ADR 0007, consequences: "we must keep bgfx as the RHI". Void. bgfx becomes one backend of several.
- [ADR 0002](0002-renderer-bgfx.md): "revisit only against a concrete wall". The reason to revisit
  is not a wall in bgfx; it is that librhi exists and is where the renderer is going.

## Context

vCAD is moving its rendering from bgfx to librhi (Metal 4, Vulkan 1.3, D3D12; explicit and
bindless; C ABI). librhi deliberately supports modern APIs only, so some machines that run vCAD
today will not run a librhi renderer: an older GPU, an old driver, a VM, a remote desktop.

**The requirement:** a user whose machine cannot run one renderer gets another, without rebuilding
and ideally without doing anything. Three are in view: librhi, bgfx, and a WebGPU one (wgpu or
Dawn).

ADR 0007's seam is most of the way there. `IGpuResources`, `IFrameSink` and `IPicker` are POD-only
and already have two implementations (bgfx and `NullBackend`). Nothing above `render/` calls bgfx.
What is not swappable is everything *around* the three interfaces:

1. **`app::Controller` owns the concrete class.** It includes `BgfxBackend.h`, holds
   `std::unique_ptr<render::BgfxBackend>`, constructs it in `View.cpp`, and hands it out from
   `renderBackend()`. It also creates the `CAMetalLayer` itself, under `#if defined(__APPLE__)`.
2. **The interface pass speaks bgfx.** `UiPass::create` takes a `BgfxBackend&`. Textures cross the
   API as "a bgfx texture index" (`std::uint16_t`): `Controller::sceneTexture()`,
   `UiPass::createImage`, `UiPass::HostTextureFn`. `UiPass` is a class, not an interface.
3. **Every shell includes `BgfxBackend.h`**, for `setShaderDirectory`, a process global.
4. **The frame's order is bgfx's.** `IFrameSink::submit` ends the frame itself, so the interface
   must be queued *before* the scene is submitted, and the picker advances frames of its own. That
   works because bgfx sorts views by id at `frame()`. An explicit API has no such sort.

## Decision 1 — swap at start-up, with a fallback chain; not mid-session

A renderer is chosen once, when the first window attaches. Switching means restarting the
application (or, later, re-attaching: `Controller` already rebuilds its `SceneBuilder` against a new
backend on attach, so nothing in this design forbids it).

Hot-swapping with a document open is **not** a goal. bgfx is a process singleton, every buffer id
belongs to the backend that issued it, and the user's problem is "it does not work on my machine",
which is a start-up problem.

The order tried:

1. What was asked for explicitly: `--renderer <id>`, then `CAD_RENDERER`, then the saved preference.
   An explicit choice that fails is an **error shown to the user**, not a silent fallback: someone
   who asked for bgfx and got librhi has been lied to.
2. Otherwise each compiled-in renderer in priority order (librhi, then WebGPU, then bgfx), skipping
   any that reports itself unavailable. The first that initialises *and presents a frame* wins.
3. If none does, the shell says so, with every renderer's reason.

Two rules carried over from what bgfx already cost this project:

- **A renderer that draws nothing has failed.** bgfx's Noop is refused today in `View.cpp`; that
  check becomes part of the contract for every backend.
- **A crash during start-up must not repeat.** A driver can take the process down inside device
  creation, where no error is returned. Before trying a renderer the application writes a marker
  beside its settings ("trying librhi"); it clears it after the first presented frame. A marker
  found at start-up means that renderer killed the last launch, so it is skipped and the user is
  told. Without this, a machine where librhi crashes can never reach bgfx.

What was chosen, and why the others were not, is always visible: the log, About, and
`Controller::rendererInfo()`.

## Decision 2 — one owning interface beside the three seam interfaces

The three interfaces stay exactly as they are. What is added is the object that owns them:

```cpp
// render/include/cad/render/Renderer.h — no backend type appears in it
struct NativeSurface {             // what the SHELL has; each backend makes what it needs from it
    void* view = nullptr;          // NSView / UIView / HWND / X11 Window / wl_surface
    void* display = nullptr;       // X11 Display / wl_display
    std::uint32_t width = 0, height = 0;   // pixels
    double scale = 1.0;
};

struct RendererConfig {
    NativeSurface surface;         // view == nullptr: offscreen
    Viewport viewport;             // the scene's own target
    bool sceneToTexture = false;   // a shell that draws the viewport as one widget
    std::string shaderDirectory;   // replaces the setShaderDirectory global
    float edgeDepthBias = 1.5e-4f;
    float ambient = 0.35f;
};

struct RendererInfo {
    std::string id;                // "librhi", "bgfx", "wgpu", "null"
    std::string api;               // "Metal 4", "Vulkan 1.3", "Direct3D 12", ...
    std::string device;            // the adapter's name
    bool homogeneousDepth = false; // feeds CameraController, as today
    bool presents = false;
};

class IRenderer {
public:
    virtual ~IRenderer() = default;
    [[nodiscard]] virtual Backend handle() noexcept = 0;        // resources, frames, picker
    [[nodiscard]] virtual RendererInfo info() const = 0;
    [[nodiscard]] virtual TextureId sceneTexture() const = 0;   // None unless sceneToTexture
    virtual void resizeSurface(std::uint32_t width, std::uint32_t height) = 0;
    virtual kernel::Result<std::vector<std::uint8_t>> captureFrame() = 0;
    [[nodiscard]] virtual IUiPass* ui() noexcept = 0;           // null: cannot draw an interface
    virtual void present() = 0;                                 // Decision 4
};
```

`NativeSurface` carries the shell's **view**, never a layer or a swap chain. bgfx wants a
`CAMetalLayer`, librhi's present module wants its own thing, WebGPU wants a surface descriptor;
turning a view into any of those is the backend's work. `MetalSurface.h` stops being public and
`Controller` loses its `#if defined(__APPLE__)`.

`Controller` holds `std::unique_ptr<render::IRenderer>` and includes `Renderer.h` only.
`renderBackend()` is deleted; `sceneTexture()` returns `render::TextureId`.

## Decision 3 — a registry of compiled-in renderers

```cpp
struct RendererFactory {
    const char* id;
    const char* label;                                   // for a settings page
    int priority;                                        // higher is tried first
    bool (*available)(std::string* why);                 // cheap: no device is created
    kernel::Result<std::unique_ptr<IRenderer>> (*create)(const RendererConfig&);
};

std::span<const RendererFactory> renderers();

struct RendererAttempt { std::string id; bool ok; std::string reason; };
kernel::Result<std::unique_ptr<IRenderer>> createRenderer(
    const RendererConfig&, std::string_view preferred, std::vector<RendererAttempt>* attempts);
```

Which factories exist is decided by CMake: `CAD_RENDERER_BGFX`, `CAD_RENDERER_LIBRHI`,
`CAD_RENDERER_WGPU`. Each backend is its own static library (`cad_render_bgfx`, ...) so a build
without bgfx does not link bgfx. A plain table, not self-registering statics: a static library's
unreferenced object files are dropped by the linker, and a renderer that silently is not there is
this project's least favourite kind of bug.

A shell lists `renderers()` in its settings; it never names one in code.

## Decision 4 — the frame has an explicit end

`IFrameSink::submit` records the scene and no longer ends the frame; `IRenderer::present()` does.
A frame becomes: scene `submit`, interface `ui()->submit` per window, `present()`, in that order,
on every backend. The picker stops advancing frames behind the caller's back and reads back through
an explicit call instead.

This is the one change that touches the existing seam, and it is needed because "the interface must
be queued before the scene, because the scene's submit is also the present" is an accident of
bgfx's view sorting that librhi and WebGPU cannot reproduce. `NullBackend` gains a no-op `present`.

## Decision 5 — the interface pass is an interface, and textures are opaque

```cpp
class IUiPass {
public:
    virtual ~IUiPass() = default;
    enum class Geometry { Mesh, Instances };
    [[nodiscard]] virtual Geometry wants() const = 0;
    virtual kernel::Result<void> setLayout(std::span<const UiAttribute>, std::uint32_t stride) = 0;
    virtual void submit(const UiFrame&, SurfaceId, std::uint32_t pxWidth, std::uint32_t pxHeight) = 0;
    virtual kernel::Result<SurfaceId> addSurface(const NativeSurface&) = 0;   // a torn-off window
    virtual void removeSurface(SurfaceId) = 0;
    virtual TextureId createImage(std::span<const std::uint8_t> rgba, std::uint32_t w, std::uint32_t h) = 0;
    using HostTextureFn = TextureId (*)(std::uint64_t texture, void* user);
    virtual void setHostTextures(HostTextureFn, void* user) = 0;
};
```

`TextureId` already exists in `Backend.h` and is unused; it becomes the only way a texture is
named above `render/`. `wants()` exists because bgfx cannot carry libgui's six-attribute instance
and needs the 4.5x triangle form, while librhi and WebGPU can take the instances directly; the
shell asks libgui for whichever the renderer wants.

## Decision 6 — "swappable" is a test, not a property of the headers

Two implementations kept the three interfaces honest. Four will not stay interchangeable by
intention. One conformance suite runs against **every compiled-in renderer**:

- the seam contract tests that run against `NullBackend` today;
- **pixel tests**: a fixed scene (several parts, several transforms, a selection, a section plane)
  rendered through `captureFrame`, compared against a committed reference within a tolerance, and
  compared across backends. ADR 0007's rule stands: a rendering claim is not established by a
  counter;
- pick tests: the same pixel resolves to the same element on every backend;
- the interface pass against libgui's CPU reference renderer (`libgui_reference_pixels`).

A renderer that fails the suite on a machine is, by Decision 1, unavailable on that machine.

Guards, so the leaks do not come back: `check_layering.py` forbids every `cad/render/<Backend>*.h`
above `render/`, and the audit gains a rule that a renderer is constructed in `createRenderer` only.

## Shaders

Seven shaders today (five scene, two interface), in bgfx's dialect. Three renderers means three
dialects: bgfx `.sc`, Slang for librhi, WGSL for WebGPU. Proposed: Slang is the source for librhi
and WebGPU (it emits WGSL), bgfx keeps its hand-written `.sc`, and the pixel tests are what say the
two agree. Compiled shaders live in `shaders/<renderer id>/`.

## Order

Each step is one commit, bgfx-only and behaviour-preserving until the last:

1. `TextureId` replaces the `uint16_t` texture index everywhere above `render/`.
2. `IRenderer`, `RendererConfig`, the registry with bgfx as its only entry; `Controller` and the
   three shells move to it. `setShaderDirectory` goes.
3. `IUiPass`, with the bgfx pass behind it.
4. The explicit `present()`.
5. `NativeSurface`: the layer moves from `Controller` into the bgfx backend.
6. The conformance suite, parameterised over `renderers()`, with the first pixel references.
7. The two guards.
8. The fallback chain and the start-up marker, tested with a renderer built to fail.

Then the librhi backend, written against a seam that no longer mentions bgfx.

## Open questions

1. **Which WebGPU?** wgpu is Rust; from C++ it is `wgpu-native` behind `webgpu.h`, or Dawn behind
   the same header. librhi's planned tier 1 is WebGPU-level and may make this backend unnecessary.
2. **Is bgfx kept permanently**, or only until librhi's reach is known? It is the only one of the
   three that runs on OpenGL and D3D11.
3. **The iPad.** librhi runs there. Does it get the same chain, or one renderer only?
4. **Does the preference belong to the user or the machine?** A setting synced between a new Mac and
   an old PC should not carry "librhi" to the PC.
5. **The pixel tolerance.** Three rasterisers will not agree to the bit on anti-aliased edges; the
   number has to be chosen from measurement, not guessed here.
