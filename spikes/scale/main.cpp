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
#include "cad/features/Builtins.h"
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
#include <optional>
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
    recompute::FeatureRegistry registry = features::builtins();
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
    const std::size_t rebuildsBefore = scene.stats().rebuilds;
    auto rebuildStart = Clock::now();
    (void)scene.update(doc, placements);
    const double rebuildMs = msSince(rebuildStart);
    const std::size_t noopRebuilds = scene.stats().rebuilds - rebuildsBefore;
    std::printf("no-op update: %8.3f ms   (%zu rebuilds; the cost is the placement digest, which "
                "is O(placements) and unavoidable)\n",
                rebuildMs, noopRebuilds);

    // Bounds, printed because they are the other explanation for "one box on screen": if they
    // cover a single instance, the camera frames that one and the rest are simply outside the
    // viewport. Identical symptom, entirely different bug.
    {
        const auto b = scene.bounds();
        std::printf("bounds: (%.1f %.1f %.1f) .. (%.1f %.1f %.1f)\n", b.min[0], b.min[1],
                    b.min[2], b.max[0], b.max[1], b.max[2]);
    }

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
    std::printf("camera-only:  %8.4f ms/change (must be ~0: no instance rebuild)\n", camMs);

    // Culling, measured where it matters: zoomed IN, with most of the assembly off screen. With
    // the whole model framed nothing should be culled, so a fitted camera proves nothing.
    for (int i = 0; i < 60; ++i) camera.zoom(1.0f);
    scene.setCamera(camera.matrices(config.viewport));
    const auto zoomed = scene.cullStats();
    std::printf("culled:       %8zu of %zu instances drawn, %zu of %zu cells, %zu ranges, "
                "%.3f ms\n\n",
                zoomed.instancesVisible, zoomed.instancesTotal, zoomed.cellsVisible,
                zoomed.cells, zoomed.ranges, zoomed.lastCullMs);

    // Back to the fitted camera for the frame timings below, so they measure the whole scene.
    camera.fit(scene.bounds(), config.viewport);
    scene.setCamera(camera.matrices(config.viewport));

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

    // 1. Draw calls proportional to unique meshes, and MORE THAN NONE.
    //
    //    The lower bound is not pedantry. This printed "PASS draw calls 0 <= 16000" at 8,000 unique
    //    parts, while the renderer was drawing absolutely nothing: bgfx's buffer handle pool is
    //    fixed at 4096, vCAD takes two vertex buffers per mesh, and past ~2048 meshes every upload
    //    failed and every batch was skipped. An upper bound is satisfied by zero, so the headline
    //    claim of this whole spike went green exactly when the renderer stopped working — and the
    //    frame rate went UP, which made it look like an improvement.
    const std::uint32_t ceiling = uniqueParts * 2;
    if (s.drawCalls == 0 || s.triangles == 0) {
        std::fprintf(stderr,
                     "FAIL: nothing was drawn (%u calls, %llu triangles). The scene believes it has "
                     "geometry, so this is a renderer failure, not an empty document.\n",
                     s.drawCalls, static_cast<unsigned long long>(s.triangles));
        ++failures;
    } else if (s.drawCalls > ceiling) {
        std::fprintf(stderr,
                     "FAIL: %u draw calls for %u unique parts (expected <= %u). Dedupe or "
                     "batching is not working.\n",
                     s.drawCalls, uniqueParts, ceiling);
        ++failures;
    } else {
        std::printf("PASS  draw calls %u in (0, %u] (2 passes x %u unique meshes), independent of "
                    "%u instances\n",
                    s.drawCalls, ceiling, uniqueParts, instanceCount);
    }

    // 1b. Nothing was quietly dropped on the way to the GPU. The backend counts a batch whose
    //     buffers do not resolve; it used to `continue` in silence, which is how an empty frame
    //     passed for a fast one.
    if (s.skippedBatches > 0) {
        std::fprintf(stderr,
                     "FAIL: %u batches were skipped because their buffers did not resolve. That is "
                     "geometry the scene thinks is on screen and the user cannot see.\n",
                     s.skippedBatches);
        ++failures;
    } else {
        std::printf("PASS  no batches skipped for missing buffers\n");
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

    // 4. An unchanged document must not rebuild.
    //
    //    Counted, not timed. This was a ratio against the build time, and it started failing the
    //    moment the build got fast — 10 ms of unavoidable digest walk against a 41 ms build is
    //    more than a quarter of it, and says nothing about whether the early-out fired. The
    //    rebuild counter answers the actual question; the timing above is reported, not asserted.
    if (noopRebuilds != 0) {
        std::fprintf(stderr,
                     "FAIL: re-updating an unchanged document performed %zu rebuild(s). The "
                     "early-out is not firing.\n",
                     noopRebuilds);
        ++failures;
    } else {
        std::printf("PASS  no-op update rebuilt nothing (%.3f ms, all of it the digest compare)\n",
                    rebuildMs);
    }

    // 5. Culling must actually cull. A zoomed-in camera sees a small fraction of a 100k-part
    //    grid, and if the same instance count still reaches the GPU then the cell tests are
    //    running and deciding nothing — which is worse than not culling, because it costs.
    //
    //    Only meaningful with more than one cell. Culling decides per CELL, so a scene small
    //    enough to fit in one bucket is all-or-nothing and a zoomed-in camera sitting inside that
    //    bucket correctly keeps every instance. Asserting here anyway is how this reported
    //    "frustum culling is not rejecting anything" for every run at n=8, against a culler that
    //    is fine: at n=512 it draws 128 of 512, at n=4096 it draws 640 of 4096.
    //
    //    The gate is cells PER BATCH, not cells overall. Cells are spatial bins WITHIN a batch,
    //    so a batch holding fewer than a cell's worth of instances gets exactly one cell -- and
    //    that cell spans wherever those instances are, which for a scattered mesh is the entire
    //    assembly. Nothing can cull it, correctly. A first version gated on total cells and so
    //    still fired at 512 instances of 20 meshes: 20 cells, one per batch, none cullable.
    if (zoomed.cells < 2 * static_cast<std::size_t>(uniqueParts)) {
        std::printf("SKIP  culling not exercised: %zu cells for %u batches, so no batch has "
                    "more than one spatial bin (raise n)\n", zoomed.cells, uniqueParts);
    } else if (zoomed.instancesVisible >= zoomed.instancesTotal) {
        std::fprintf(stderr,
                     "FAIL: zoomed in, culling still drew all %zu instances. Frustum culling is "
                     "not rejecting anything.\n",
                     zoomed.instancesTotal);
        ++failures;
    } else {
        std::printf("PASS  zoomed in, culling drew %zu of %zu instances in %zu draw ranges\n",
                    zoomed.instancesVisible, zoomed.instancesTotal, zoomed.ranges);
    }

    // PIXEL CHECK. ADR 0007's rule, applied to the spike that most needed it: a scale claim is not
    // established by a counter. Counters said 8 instances for the entire life of the instancing
    // bug; only the framebuffer knows how many boxes are actually there.
    //
    // CALIBRATED against a baseline frame, because a self-referential pixel statistic is how this
    // check failed to be a check. It used to count distinct horizontal spans of lit pixels, on the
    // theory that N boxes on a grid occupy N spans and N boxes stacked at one transform occupy
    // one. Both halves are wrong. Adjacent boxes merge into a single run, so a correct frame of 8
    // boxes reported ONE span and read as the failure; and 3 spans satisfied it whether the image
    // held 3 boxes or 512. It agreed with itself rather than with anything real.
    //
    // What actually has meaning is the same scene rendered twice from the SAME camera: once with
    // every placement, once with placement 0 alone. The second frame is exactly one instance's
    // footprint at this zoom, so it is the unit the first frame is measured in. If every instance
    // draws at the same transform the two frames are identical, which no counter can hide and no
    // silhouette merging can disguise.
    {
        const std::uint32_t w = config.viewport.width;
        const std::uint32_t h = config.viewport.height;

        // Lit pixels in the frame currently on the backend, optionally saved as a PPM.
        const auto litPixels = [&](const char* ppm) -> std::optional<std::size_t> {
            auto pixels = backend.captureFrame();
            if (!pixels) {
                std::printf("capture failed: %s\n", pixels.error().message.c_str());
                return std::nullopt;
            }
            const auto& px = pixels.value();
            std::size_t lit = 0;
            for (std::size_t i = 0; i + 3 < px.size(); i += 4) {
                if (px[i] > 80 || px[i + 1] > 80 || px[i + 2] > 80) ++lit;
            }
            if (ppm != nullptr) {
                if (FILE* f = std::fopen(ppm, "wb")) {
                    std::fprintf(f, "P6\n%u %u\n255\n", w, h);
                    for (std::size_t i = 0; i + 3 < px.size(); i += 4) {
                        std::fputc(px[i], f);
                        std::fputc(px[i + 1], f);
                        std::fputc(px[i + 2], f);
                    }
                    std::fclose(f);
                }
            }
            return lit;
        };

        const auto litAll = litPixels("scale.ppm");

        // The baseline. Camera untouched — refitting it would zoom onto the single box and
        // measure a different footprint, which is the whole trap this is avoiding.
        std::optional<std::size_t> litOne;
        if (instanceCount > 1 && litAll) {
            // Screen-size culling OFF for the baseline. One instance viewed at a zoom that
            // frames the whole assembly is sub-pixel well before 100k parts, so minPixels
            // legitimately culls it -- correct behaviour that leaves nothing drawn, and with
            // nothing drawn the frame read back as a copy of the previous one. The measurement
            // wanted here is one instance's FOOTPRINT, and a footprint that was culled is not a
            // measurement of anything.
            render::SceneBuilder::CullSettings baseline;
            baseline.minPixels = 0.0f;
            scene.setCullSettings(baseline);

            const std::vector<render::Placement> one{placements.front()};
            if (auto r = scene.update(doc, one); !r) {
                std::printf("baseline update failed: %s\n", r.error().message.c_str());
            } else {
                gpu.frames->submit(scene.frame());
                const auto st = gpu.frames->lastFrameStats();
                std::printf("baseline: scene says %zu instances, backend drew %u in %u calls\n",
                            scene.stats().instances, st.instances, st.drawCalls);
                litOne = litPixels("scale_one.ppm");
            }
        }

        if (!litAll) {
            ++failures;
        } else if (*litAll == 0) {
            std::fprintf(stderr, "FAIL: nothing rasterised\n");
            ++failures;
        } else if (instanceCount == 1) {
            std::printf("PASS  %zu lit pixels for a single instance\n", *litAll);
        } else if (!litOne) {
            std::fprintf(stderr, "FAIL: could not render the one-instance baseline to compare "
                                 "against\n");
            ++failures;
        } else if (*litOne == 0) {
            // Genuinely sub-pixel: at this instance count one part covers less than a whole
            // pixel at the fitted camera, so there is no unit to measure in. Skipped rather
            // than failed -- an unmeasurable scene is not a broken one.
            std::printf("SKIP  one instance is sub-pixel at this zoom; no baseline to calibrate "
                        "against (lower n to check transforms)\n");
        } else {
            // Occlusion means N instances never cover N times one instance's pixels -- on an
            // isometric grid the front boxes hide much of what is behind them -- so the bar is
            // deliberately low and absolute rather than proportional to N. Stacked instances give
            // a ratio of exactly 1.00. Anything at or below 1.5 is the bug or close enough to it
            // to be worth a human looking at scale.ppm and scale_one.ppm side by side.
            const double ratio = double(*litAll) / double(*litOne);
            std::printf("pixels: %zu lit for %u instances, %zu for one at the same camera "
                        "(x%.2f)\n", *litAll, instanceCount, *litOne, ratio);
            if (ratio <= 1.5) {
                std::fprintf(stderr,
                             "FAIL: %u instances cover only %.2fx the pixels of ONE instance at "
                             "the same camera. They are drawing at the same transform.\n",
                             instanceCount, ratio);
                ++failures;
            } else {
                std::printf("PASS  %u instances cover %.2fx a single instance's footprint: they "
                            "are at distinct transforms\n", instanceCount, ratio);
            }
        }
    }

    backend.shutdown();
    if (failures != 0) {
        std::fprintf(stderr, "\n%d claim(s) failed.\n", failures);
        return 1;
    }
    std::printf("\nAll claims hold at %u instances.\n", instanceCount);
    return 0;
}
