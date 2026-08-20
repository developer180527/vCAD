/// The viewport: camera, renderer attachment, presentation and frame statistics.
///
/// Split out of Controller.cpp, which had reached 2574 lines. The class is unchanged --
/// these are the same methods in the same order, moved verbatim into a file named for what
/// they do, so the system can be read one concern at a time.

#include "Internal.h"

#include "cad/io/Format.h"
#include "cad/kernel/Primitives.h"

#include "cad/render/MetalSurface.h"

#include "cad/io/DocumentStore.h"
#include "cad/sketch/Sketch.h"

#include "cad/units/Units.h"

#include <sstream>
#include <tuple>

#include <algorithm>
#include <chrono>


namespace cad::app {

void Controller::setViewportSize(std::uint32_t width, std::uint32_t height) {
    if (width == 0 || height == 0) return;
    viewport_.width = width;
    viewport_.height = height;
    scene_->setViewport(viewport_);
    scene_->setCamera(camera_.matrices(viewport_));
    // The backend owns the framebuffer being read back, so it has to be told too. Skipping this
    // reads back the OLD size and the shell blits a stale, wrongly-shaped image.
#if defined(__APPLE__)
    // Before the backend's resize: bgfx::reset builds a swap chain against the layer, and a layer
    // still at the old drawableSize gives a surface that disagrees with the backbuffer.
    if (surface_ != nullptr) render::resizeMetalLayer(surface_, width, height, viewportScale_);
#endif
    if (active_.frames != nullptr) active_.frames->resize(viewport_);
    notifyView();
}

void Controller::cameraChanged() {
    scene_->setCamera(camera_.matrices(viewport_));
    // The slice normal faces the camera, so orbiting past the plane has to flip it — otherwise
    // orbiting to the other side shows the half that was just cut away and hides the half you
    // were working on.
    if (slice_) applySlice();
    notifyView();
}

void Controller::fitView() {
    camera_.fit(scene_->bounds(), viewport_);
    scene_->setCamera(camera_.matrices(viewport_));
    notifyView();
}

void Controller::setViewportBackground(int r, int g, int b) {
    scene_->setBackground(float(r) / 255.0f, float(g) / 255.0f, float(b) / 255.0f);
    notifyView();
}

// ── the GPU renderer ────────────────────────────────────────────────────────────────────

kernel::Result<void> Controller::attachRenderer(std::uint32_t width, std::uint32_t height,
                                                void* nativeView, double scale) {
    // bgfx is a process singleton, so this is idempotent rather than an error: two viewports
    // share one backend and differ by view id.
    if (gpu_) return {};

    // A failed attempt must not leak into the next one. The shell's fallback retries with
    // nativeView = nullptr expecting the offscreen path, and a surface left over from the native
    // attempt would silently rebuild the SAME on-screen configuration — so the fallback would
    // fail identically, or succeed on-screen while the shell believed it was blitting.
    releaseSurface();

    auto gpu = std::make_unique<render::BgfxBackend>();
    const std::uint32_t w = width != 0 ? width : viewport_.width;
    const std::uint32_t h = height != 0 ? height : viewport_.height;

    // Outside the platform guard: every platform needs the scale for later layer/backbuffer
    // resizes, and keeping it Apple-only means Windows and X11 silently render at 1.0 on a
    // high-DPI display.
    viewportScale_ = scale;
    // Into the VIEWPORT as well, not only kept beside it.
    //
    // `Viewport::devicePixelRatio` existed and was never assigned, so every consumer read 1.0 on
    // every platform — a field that describes the display and always lies about it. The selection
    // halo scales by it, which is how the omission surfaced: the highlight came out a half-point
    // hair on exactly the Retina screens it was widened for.
    viewport_.devicePixelRatio = static_cast<float>(scale);

#if defined(__APPLE__)
    // A CAMetalLayer, never the view: bgfx::init parks the calling thread waiting for the render
    // thread, and given a view the render thread must build the layer on the main thread and
    // wait for it. Both wait forever. See render/src/MetalSurface.mm.
    if (nativeView != nullptr) surface_ = render::createMetalLayerForView(nativeView, w, h, scale);
#else
    // Windows and X11 take the window handle directly; no intermediate surface object.
    surface_ = nativeView;
#endif

    render::BgfxConfig config;
    config.nativeWindow = surface_;
    // Mutually exclusive, and bgfx enforces it: offscreen init demands a 0x0 backbuffer, so a
    // surface handle and offscreen mode cannot both be set.
    config.offscreen = surface_ == nullptr;
    config.viewport.width = w;
    config.viewport.height = h;
    // The backend needs the display's scale, not only its pixel count: the selection halo is sized
    // in logical pixels and expands by this.
    config.viewport.devicePixelRatio = static_cast<float>(scale);

    if (auto r = gpu->initialise(config); !r) {
        releaseSurface();
        return r.error();
    }

    // Noop is not a failure to bgfx: it validates every call and draws nothing, so init succeeds
    // and the frame comes back blank with no error anywhere. Refuse it here instead, because a
    // viewport that silently renders nothing is the single most expensive bug this project has
    // had, twice.
    if (gpu->rendererName() == "Noop") {
        gpu->shutdown();
        releaseSurface();
        return kernel::Error{kernel::ErrorCode::Internal,
                             "No GPU renderer available; the viewport would draw nothing.",
                             "bgfx selected the Noop renderer"};
    }

    gpu_ = std::move(gpu);
    active_ = gpu_->handle();
    presenting_ = surface_ != nullptr;

    // Before any camera matrix is built. The two conventions differ by renderer, and getting it
    // wrong depth-clips the whole scene and draws nothing — with no error.
    camera_.setHomogeneousDepth(gpu_->homogeneousDepth());

    // A SceneBuilder holds buffer ids issued by the resources it was built against, so it cannot
    // be pointed at a different backend. Rebuild it and re-upload; at startup the document is
    // usually empty, and when it is not this is a one-off cost at attach.
    scene_ = std::make_unique<render::SceneBuilder>(*meshes_, *active_.resources);
    viewport_ = config.viewport;
    scene_->setViewport(viewport_);
    active_.frames->resize(viewport_);
    if (auto r = scene_->update(history_.current(), placements_); !r) return r.error();
    scene_->setCamera(camera_.matrices(viewport_));

    notifyView();
    return {};
}

std::string Controller::rendererName() const {
    return gpu_ ? gpu_->rendererName() : std::string("null");
}

void Controller::presentFrame() {
    if (!gpu_ || !presenting_) return;
    const auto t0 = std::chrono::steady_clock::now();
    active_.frames->submit(scene_->frame());
    timing_.submitMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    timing_.captureMs = 0.0;   // the entire point of this path
}

kernel::Result<Controller::RenderedFrame> Controller::renderFrame() {
    if (!gpu_) {
        return kernel::Error{kernel::ErrorCode::InvalidInput,
                             "No renderer is attached.",
                             "call attachRenderer() first"};
    }
    const auto t0 = std::chrono::steady_clock::now();
    active_.frames->submit(scene_->frame());
    const auto t1 = std::chrono::steady_clock::now();
    auto pixels = gpu_->captureFrame();
    const auto t2 = std::chrono::steady_clock::now();
    using Ms = std::chrono::duration<double, std::milli>;
    timing_.submitMs = Ms(t1 - t0).count();
    timing_.captureMs = Ms(t2 - t1).count();
    if (!pixels) return pixels.error();

    RenderedFrame out;
    out.width = viewport_.width;
    out.height = viewport_.height;
    out.pixels = std::move(pixels.value());
    return out;
}

Controller::Stats Controller::stats() const {
    Stats s;
    s.objects = history_.current().size();
    s.uniqueMeshes = scene_->stats().uniqueMeshes;
    s.instances = scene_->stats().instances;
    s.failed = failedCount_;
    for (const auto& batch : scene_->frame().batches) {
        // instanceCount, not the visible ranges: this is the status bar's "how big is this
        // model" figure, and a number that dropped every time the user orbited would read as
        // geometry going missing.
        s.triangles += (batch.indexCount / 3) * batch.instanceCount;
    }
    return s;
}

// ── commands ────────────────────────────────────────────────────────────────────────────

}  // namespace cad::app
