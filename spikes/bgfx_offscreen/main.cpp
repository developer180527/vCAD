// Spike: prove the bgfx backend initialises on real hardware, uploads a real mesh, submits a
// frame and reads pixels back — with no window.
//
// This is the only part of M3.3 that can be verified without a human looking at a screen, which
// is exactly why offscreen mode exists. It is a spike rather than a test because CI has no GPU;
// see docs/M3.md.

#include "cad/document/Document.h"
#include "cad/features/Builtins.h"
#include "cad/recompute/Engine.h"
#include "cad/render/BgfxBackend.h"
#include "cad/render/Camera.h"
#include "cad/render/Scene.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

using namespace cad;

int main(int argc, char** argv) {
    // Unbuffered stdout. Diagnostics that vanish when the process hangs are worse than no
    // diagnostics: a block-buffered pipe swallowed every printf below and the run looked like
    // it had produced nothing at all, when in fact it had produced everything and then hung.
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // Staged progress markers. "The program printed nothing" is the least useful bug report
    // possible, and it is what this spike produced twice: once because stdout was block-buffered
    // into a pipe, and once because it hung before the first diagnostic. Every step that can
    // block now announces itself BEFORE it runs, so the last line printed names the culprit.
    const auto step = [](const char* what) { std::printf("[step] %s\n", what); };
    step("start");

    const bool noop = argc > 1 && std::string(argv[1]) == "--noop";

    render::BgfxConfig config;
    config.viewport.width = 512;
    config.viewport.height = 384;
    config.offscreen = true;
    if (noop) config.rendererName = "noop";

    // NO WINDOW, deliberately, and this is load-bearing.
    //
    // The previous version created a hidden GLFW window because I believed bgfx could not make a
    // Metal device without a surface. It can — MTLCreateSystemDefaultDevice() takes no surface —
    // and passing a window handle here DEADLOCKED bgfx::init on macOS every time: the render
    // thread tried to create the window's CAMetalLayer via the main run loop while the main
    // thread was blocked inside bgfx::init waiting for the render thread. Diagnosed from a stack
    // sample; see the platformData comment in render/src/BgfxBackend.cpp.
    //
    // Dropping the window also removes the ssh refusal that used to guard this spike, so it now
    // runs anywhere with a GPU and no window server at all.

    step("bgfx init");
    render::BgfxBackend backend;
    if (auto r = backend.initialise(config); !r) {
        std::fprintf(stderr, "init failed: %s (%s)\n", r.error().message.c_str(),
                     r.error().detail.c_str());
        return 1;
    }
    std::printf("renderer: %s   homogeneousDepth: %s   programs: %s\n",
                backend.rendererName().c_str(),
                backend.homogeneousDepth() ? "true (OpenGL [-1,1])" : "false (Metal/D3D [0,1])",
                backend.programsReady() ? "loaded" : "MISSING");
    if (!noop && backend.rendererName() == "Noop") {
        std::fprintf(stderr,
                     "FAIL: bgfx fell back to Noop — it could not create a real device.\n");
        return 1;
    }

    // A box, recomputed, tessellated, batched, submitted.
    recompute::FeatureRegistry registry = features::builtins();
    recompute::MemoryCache cache;
    recompute::MemoryBlobStore blobs;
    render::MeshCache meshes(blobs);

    auto [doc, id] = document::Document{}.add("Box");
    for (const char* name : {"dx", "dy", "dz"}) {
        const auto object = doc.find(id);
        doc = doc.replace(std::make_shared<const document::ObjectData>(
            object->withProperty(name, units::millimetres(name[1] == 'z' ? 40.0 : 100.0))));
    }
    recompute::Engine engine(registry, cache);
    auto computed = engine.recompute(doc);
    if (!computed) {
        std::fprintf(stderr, "recompute failed: %s\n", computed.error().message.c_str());
        return 1;
    }
    doc = computed.value().first;

    auto gpu = backend.handle();
    render::SceneBuilder scene(meshes, *gpu.resources);
    render::Placement placement;
    placement.object = id;
    if (auto r = scene.update(doc, {&placement, 1}); !r) {
        std::fprintf(stderr, "scene failed: %s\n", r.error().message.c_str());
        return 1;
    }

    render::CameraController camera;
    // The depth convention MUST come from the renderer. Assuming OpenGL's [-1,1] on Metal
    // depth-clips every fragment and the frame comes out empty with no error at all.
    camera.setHomogeneousDepth(backend.homogeneousDepth());
    camera.fit(scene.bounds(), config.viewport);
    scene.setViewport(config.viewport);
    scene.setCamera(camera.matrices(config.viewport));

    const auto b = scene.bounds();
    std::printf("bounds: (%.1f %.1f %.1f) .. (%.1f %.1f %.1f)   camera distance %.1f\n",
                b.min[0], b.min[1], b.min[2], b.max[0], b.max[1], b.max[2],
                camera.distance());
    std::printf("scene: %zu unique meshes, %zu instances, %zu element slots\n",
                scene.stats().uniqueMeshes, scene.stats().instances, scene.stats().elementSlots);

    // Two frames. The first lets bgfx create the framebuffer and flush its deferred resource
    // creation; capturing after a single frame reads back an untouched texture.
    gpu.frames->submit(scene.frame());
    gpu.frames->submit(scene.frame());
    const auto stats = gpu.frames->lastFrameStats();
    std::printf("draw calls: %u  instances: %u  triangles: %u  lines: %u\n", stats.drawCalls,
                stats.instances, stats.triangles, stats.lines);
    std::printf("resident: %llu bytes\n",
                static_cast<unsigned long long>(gpu.resources->residentBytes()));

    if (noop) {
        // Noop has no shader profile, so no program loads and nothing can be submitted.
        // What this mode proves is narrower and still worth having: init, tessellation, scene
        // assembly and buffer creation all work. Requiring draw calls here was simply wrong.
        std::printf("noop: init + scene + buffers OK (no program, so no draws)\n");
        return gpu.resources->residentBytes() > 0 ? 0 : 1;
    }

    // Read the framebuffer back and check something actually rasterised: the background is a
    // known colour, so any pixel differing from it is geometry.
    auto pixels = backend.captureFrame();
    if (!pixels) {
        std::fprintf(stderr, "capture failed: %s\n", pixels.error().message.c_str());
        return 1;
    }
    // Distinguish the two ways this can read as "nothing": geometry that never drew, versus a
    // readback that never happened. They look identical in a pixel count and need completely
    // different fixes, so measure them apart.
    //
    //   background-coloured pixels  -> the clear reached the texture, so the readback works and
    //                                  the geometry is the problem (camera, depth, shader).
    //   all-zero pixels             -> we are reading an untouched texture; the blit or the
    //                                  readback failed and the draw is unmeasured.
    std::size_t nonBackground = 0;
    std::size_t backgroundish = 0;
    std::size_t zero = 0;
    std::uint8_t lo[3]{255, 255, 255};
    std::uint8_t hi[3]{0, 0, 0};
    for (std::size_t i = 0; i + 3 < pixels.value().size(); i += 4) {
        const auto* p = &pixels.value()[i];
        for (int c = 0; c < 3; ++c) {
            lo[c] = std::min(lo[c], p[c]);
            hi[c] = std::max(hi[c], p[c]);
        }
        if (p[0] == 0 && p[1] == 0 && p[2] == 0) ++zero;
        else if (p[0] < 60 && p[1] < 60 && p[2] < 60) ++backgroundish;
        if (p[0] > 80 || p[1] > 80 || p[2] > 80) ++nonBackground;
    }
    const std::size_t total = pixels.value().size() / 4;
    std::printf("pixel range: r %u..%u  g %u..%u  b %u..%u\n", lo[0], hi[0], lo[1], hi[1],
                lo[2], hi[2]);
    std::printf("all-zero: %zu   background-ish: %zu   lit: %zu   of %zu\n", zero, backgroundish,
                nonBackground, total);
    if (zero == total) {
        std::fprintf(stderr,
                     "DIAGNOSIS: every pixel is zero, so the clear never reached this texture.\n"
                     "           The blit or the readback failed — not the geometry.\n");
    } else if (nonBackground == 0 && backgroundish > 0) {
        std::fprintf(stderr,
                     "DIAGNOSIS: the clear colour is present but no geometry is.\n"
                     "           Readback works; the draw is being lost (camera, depth or shader).\n");
    }
    std::printf("lit pixels: %zu / %zu (%.1f%%)\n", nonBackground, total,
                100.0 * double(nonBackground) / double(total));

    // Always write the image, pass or fail. A number saying "0 lit pixels" tells you it is
    // broken; the picture tells you HOW — wrong camera, wrong winding, wrong clear colour.
    if (FILE* f = std::fopen("frame.ppm", "wb")) {
        std::fprintf(f, "P6\n%u %u\n255\n", config.viewport.width, config.viewport.height);
        for (std::size_t i = 0; i + 3 < pixels.value().size(); i += 4) {
            std::fputc(pixels.value()[i], f);
            std::fputc(pixels.value()[i + 1], f);
            std::fputc(pixels.value()[i + 2], f);
        }
        std::fclose(f);
        std::printf("wrote frame.ppm (open it: any image viewer, or `open frame.ppm`)\n");
    }

    if (nonBackground == 0) {
        std::fprintf(stderr, "FAIL: nothing rasterised\n");
        return 1;
    }
    // A fitted box should cover a substantial part of the view but not all of it.
    if (nonBackground > total * 95 / 100) {
        std::fprintf(stderr, "FAIL: whole frame is geometry — camera or clear is wrong\n");
        return 1;
    }
    std::printf("OK\n");
    backend.shutdown();
    return 0;
}
