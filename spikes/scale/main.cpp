// Spike: does the renderer actually hold up at CAD assembly scale?
//
// The architectural claim being tested, from ADR 0007 and docs/M3.md:
//
//   Draw calls are proportional to the number of UNIQUE meshes, not to the number of parts.
//   So 250,000 bolts made from 20 distinct bolt models cost ~40 draw calls, not 250,000.
//
// Plus the two claims that make interaction survive: a camera-only change must rebuild nothing,
// and an unchanged document must rebuild nothing.
//
// This is a spike, not a test: it needs a GPU, and CI runners do not have one. It renders
// offscreen, so it needs no window and no window server — see render/src/MetalSurface.mm.
//
// Usage:  spike_scale [instances] [unique-parts]
//         spike_scale 250000 20

#include "cad/document/Document.h"
#include "cad/recompute/Engine.h"
#include "cad/render/BgfxBackend.h"
#include "cad/render/Camera.h"
#include "cad/render/Scene.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace cad;

namespace {

using Clock = std::chrono::steady_clock;

double msSince(Clock::time_point t) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

/// Median rather than mean: one frame that stalls on a driver allocation should not become the
/// headline number, and with a handful of samples the mean is dominated by it.
double median(std::vector<double> v) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    const std::uint32_t instanceCount = argc > 1 ? std::uint32_t(std::atoi(argv[1])) : 100000u;
    const std::uint32_t uniqueParts = argc > 2 ? std::uint32_t(std::atoi(argv[2])) : 20u;
    if (instanceCount == 0 || uniqueParts == 0) {
        std::fprintf(stderr, "instances and unique-parts must both be > 0\n");
        return 2;
    }

    render::BgfxConfig config;
    config.viewport.width = 1280;
    config.viewport.height = 800;
    config.offscreen = true;

    render::BgfxBackend backend;
    if (auto r = backend.initialise(config); !r) {
        std::fprintf(stderr, "init failed: %s (%s)\n", r.error().message.c_str(),
                     r.error().detail.c_str());
        return 1;
    }
    if (backend.rendererName() == "Noop") {
        std::fprintf(stderr, "FAIL: no real GPU device; every timing below would be a lie.\n");
        return 1;
    }
    std::printf("renderer: %s   viewport: %ux%u\n", backend.rendererName().c_str(),
                config.viewport.width, config.viewport.height);
    std::printf("target: %u instances from %u unique parts\n\n", instanceCount, uniqueParts);

    // ── Build the document: `uniqueParts` distinct shapes ────────────────────────────────
    //
    // Distinct DIMENSIONS, so their content hashes differ and dedupe cannot collapse them. That
    // is the honest version of this test: identical parts would make the draw-call claim trivial.
    recompute::FeatureRegistry registry = recompute::FeatureRegistry::builtins();
    recompute::MemoryCache cache;
    recompute::MemoryBlobStore blobs;
    render::MeshCache meshes(blobs);

    auto buildStart = Clock::now();
    document::Document doc;
    std::vector<document::ObjectId> parts;
    parts.reserve(uniqueParts);
    for (std::uint32_t i = 0; i < uniqueParts; ++i) {
        const bool cylinder = (i % 3) == 2;
        auto [next, id] = doc.add(cylinder ? "Cylinder" : "Box");
        doc = next;
        const auto object = doc.find(id);
        const double scale = 8.0 + double(i);
        auto withProps = object;
        if (cylinder) {
            withProps = std::make_shared<const document::ObjectData>(
                withProps->withProperty("radius", units::millimetres(scale * 0.5))
                    .withProperty("height", units::millimetres(scale * 2.0)));
        } else {
            withProps = std::make_shared<const document::ObjectData>(
                withProps->withProperty("dx", units::millimetres(scale))
                    .withProperty("dy", units::millimetres(scale * 1.5))
                    .withProperty("dz", units::millimetres(scale * 0.75)));
        }
        doc = doc.replace(withProps);
        parts.push_back(id);
    }

    recompute::Engine engine(registry, cache);
    auto computed = engine.recompute(doc);
    if (!computed) {
        std::fprintf(stderr, "recompute failed: %s\n", computed.error().message.c_str());
        return 1;
    }
    doc = computed.value().first;
    const double modelMs = msSince(buildStart);
    std::printf("model:        %8.1f ms   (%u features recomputed and tessellated on demand)\n",
                modelMs, uniqueParts);

    // ── Place them on a grid ─────────────────────────────────────────────────────────────
    const auto side = static_cast<std::uint32_t>(std::ceil(std::cbrt(double(instanceCount))));
    const float pitch = 40.0f;
    std::vector<render::Placement> placements;
    placements.reserve(instanceCount);
    for (std::uint32_t n = 0; n < instanceCount; ++n) {
        render::Placement p;
        p.object = parts[n % uniqueParts];
        // Column-major 4x3: three basis columns then the translation column.
        const auto x = float(n % side);
        const auto y = float((n / side) % side);
        const auto z = float(n / (side * side));
        p.transform[9] = x * pitch;
        p.transform[10] = y * pitch;
        p.transform[11] = z * pitch;
        placements.push_back(p);
    }

    auto gpu = backend.handle();
    render::SceneBuilder scene(meshes, *gpu.resources);

    auto sceneStart = Clock::now();
    if (auto r = scene.update(doc, placements); !r) {
        std::fprintf(stderr, "scene failed: %s\n", r.error().message.c_str());
        return 1;
    }
    const double sceneMs = msSince(sceneStart);
    std::printf("scene build:  %8.1f ms   (%zu unique meshes, %zu instances, %zu element slots)\n",
                sceneMs, scene.stats().uniqueMeshes, scene.stats().instances,
                scene.stats().elementSlots);

    // An unchanged document must rebuild nothing. This is what makes an idle redraw free.
    auto rebuildStart = Clock::now();
    (void)scene.update(doc, placements);
    const double rebuildMs = msSince(rebuildStart);
    std::printf("no-op update: %8.3f ms   (unchanged document must not rebuild)\n", rebuildMs);

    render::CameraController camera;
    camera.setHomogeneousDepth(backend.homogeneousDepth());
    camera.fit(scene.bounds(), config.viewport);
    scene.setViewport(config.viewport);
    scene.setCamera(camera.matrices(config.viewport));

    // A camera-only change must not touch instance data. At 1M instances that is 64 MB, and
    // rebuilding it per orbit frame is the difference between usable and not.
    auto camStart = Clock::now();
    for (int i = 0; i < 100; ++i) {
        camera.orbit(0.01f, 0.0f);
        scene.setCamera(camera.matrices(config.viewport));
    }
    const double camMs = msSince(camStart) / 100.0;
    std::printf("camera-only:  %8.4f ms/change (must be ~0: no instance rebuild)\n\n", camMs);

    // ── Frames ───────────────────────────────────────────────────────────────────────────
    constexpr int kWarmup = 3;
    constexpr int kFrames = 20;
    for (int i = 0; i < kWarmup; ++i) gpu.frames->submit(scene.frame());

    std::vector<double> cpu;
    std::vector<double> gpuMs;
    std::vector<double> wall;
    for (int i = 0; i < kFrames; ++i) {
        auto t = Clock::now();
        gpu.frames->submit(scene.frame());
        wall.push_back(msSince(t));
        const auto s = gpu.frames->lastFrameStats();
        cpu.push_back(s.cpuFrameMs);
        gpuMs.push_back(s.gpuFrameMs);
    }

    const auto s = gpu.frames->lastFrameStats();
    const double wallMedian = median(wall);
    std::printf("draw calls:   %8u     instances drawn: %u of %u requested\n", s.drawCalls,
                s.instances, s.instancesRequested);
    std::printf("triangles:    %8u     lines: %u\n", s.triangles, s.lines);
    std::printf("resident:     %8.1f MB  (GPU vertex/index buffers)\n",
                double(gpu.resources->residentBytes()) / (1024.0 * 1024.0));
    std::printf("frame wall:   %8.2f ms  median over %d frames -> %.1f fps\n", wallMedian, kFrames,
                wallMedian > 0.0 ? 1000.0 / wallMedian : 0.0);
    std::printf("frame cpu:    %8.2f ms  median (bgfx submit-to-submit)\n", median(cpu));
    std::printf("frame gpu:    %8.2f ms  median\n\n", median(gpuMs));

    // ── The claims, checked ──────────────────────────────────────────────────────────────
    int failures = 0;

    // 1. Draw calls proportional to unique meshes. Two passes per mesh (shaded + edges), so the
    //    ceiling is 2x unique meshes; anything near the instance count means dedupe is defeated.
    const std::uint32_t ceiling = uniqueParts * 2;
    if (s.drawCalls > ceiling) {
        std::fprintf(stderr,
                     "FAIL: %u draw calls for %u unique parts (expected <= %u). Dedupe or "
                     "batching is not working.\n",
                     s.drawCalls, uniqueParts, ceiling);
        ++failures;
    } else {
        std::printf("PASS  draw calls %u <= %u (2 passes x %u unique meshes), independent of %u "
                    "instances\n",
                    s.drawCalls, ceiling, uniqueParts, instanceCount);
    }

    // 2. Every requested instance was actually drawn. This is the check that stops a truncated
    //    frame from being reported as a fast one.
    if (s.instances < s.instancesRequested) {
        std::fprintf(stderr,
                     "FAIL: only %u of %u instances were submitted. The per-frame instance "
                     "buffer is exhausted, so this frame drew a fraction of the scene and its "
                     "timings mean nothing. Persistent instance buffers are the fix (M3.4).\n",
                     s.instances, s.instancesRequested);
        ++failures;
    } else {
        std::printf("PASS  all %u instances submitted, none truncated\n", s.instances);
    }

    // 3. A camera change must be essentially free.
    if (camMs > 0.05) {
        std::fprintf(stderr,
                     "FAIL: a camera-only change costs %.4f ms. It must not rebuild instance "
                     "data.\n",
                     camMs);
        ++failures;
    } else {
        std::printf("PASS  camera-only change %.4f ms — no instance rebuild\n", camMs);
    }

    // 4. An unchanged document must not rebuild. Allowed a little room for the digest compare
    //    itself, which does walk the placement list.
    if (rebuildMs > sceneMs * 0.25) {
        std::fprintf(stderr,
                     "FAIL: re-updating an unchanged document cost %.3f ms against a %.1f ms "
                     "build. The early-out is not firing.\n",
                     rebuildMs, sceneMs);
        ++failures;
    } else {
        std::printf("PASS  no-op update %.3f ms vs %.1f ms build — early-out works\n", rebuildMs,
                    sceneMs);
    }

    backend.shutdown();
    if (failures != 0) {
        std::fprintf(stderr, "\n%d claim(s) failed.\n", failures);
        return 1;
    }
    std::printf("\nAll claims hold at %u instances.\n", instanceCount);
    return 0;
}
