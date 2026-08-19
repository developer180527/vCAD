// Does a highlighted face render as ONE colour?
//
// Reported from the shell: selecting a body draws a dithered blue/grey checkerboard over its faces
// instead of a clean tint, worst over a fillet. The shader logic reads correctly and the tessellator
// says each face gets its own vertex block, so `v_ids.x` should be constant across a face and the
// lookup should return one value per face. One of those three statements is false.
//
// This is the measurement that says which. Guessing at it from the source has already produced one
// wrong hypothesis (that the varying was interpolating between different elements — it cannot, if
// the vertex blocks really are per face).
//
// A spike rather than a test because it needs a GPU, and CI has none.
//
// Usage:  spike_highlight

#include "cad/document/Document.h"
#include "cad/features/Builtins.h"
#include "cad/recompute/Engine.h"
#include "cad/render/BgfxBackend.h"
#include "cad/render/Camera.h"
#include "cad/render/Scene.h"
#include "cad/naming/ElementName.h"
#include "cad/render/Tessellate.h"
#include "cad/units/Units.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <utility>
#include <vector>

using namespace cad;

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    render::BgfxConfig config;
    config.viewport.width = 800;
    config.viewport.height = 600;
    config.offscreen = true;

    render::BgfxBackend backend;
    if (auto r = backend.initialise(config); !r) {
        std::fprintf(stderr, "init failed: %s\n", r.error().message.c_str());
        return 1;
    }
    if (backend.rendererName() == "Noop") {
        std::fprintf(stderr, "FAIL: no GPU; this spike measures pixels and cannot run headless.\n");
        return 1;
    }

    // A box, then a fillet on it — the case that was reported, because the fillet is where the
    // dithering was worst and it is also what makes the mesh's element count grow.
    document::Document doc;
    auto [withBox, boxId] = doc.add("Box");
    doc = withBox;
    doc = doc.replace(std::make_shared<const document::ObjectData>(
        doc.find(boxId)->withProperty("dx", units::millimetres(100))
            .withProperty("dy", units::millimetres(60))
            .withProperty("dz", units::millimetres(40))));

    recompute::MemoryCache cache;
    recompute::MemoryBlobStore blobs;
    const auto registry = features::builtins();
    recompute::Engine engine(registry, cache);
    auto computed = engine.recompute(doc);
    if (!computed) {
        std::fprintf(stderr, "recompute failed: %s\n", computed.error().message.c_str());
        return 1;
    }
    doc = computed.value().first;

    // THE FILLET, which is the reported case. A plain box highlights perfectly — measured — so the
    // fault needs whatever the fillet changes: more elements, and a mesh whose faces inherit names
    // from the box while the new fillet surfaces have names of their own.
    const auto* boxOut = doc.find(boxId)->output();
    std::vector<naming::ElementName> edges;
    if (boxOut != nullptr) {
        for (const auto& n : boxOut->map.allNames()) {
            const auto resolved = boxOut->map.resolve(n);
            if (!resolved) continue;
            if (resolved->type() == kernel::ShapeType::Edge) edges.push_back(n);
        }
    }
    std::printf("box elements: %zu, of which edges: %zu\n",
                boxOut != nullptr ? boxOut->map.allNames().size() : 0, edges.size());

    document::ObjectId shown = boxId;
    if (!edges.empty()) {
        auto [withFillet, filletId] = doc.add("Fillet");
        doc = withFillet;
        doc = doc.replace(std::make_shared<const document::ObjectData>(
            doc.find(filletId)->withProperty("radius", units::millimetres(4))
                .withProperty("edges", edges)
                .withProperty("a_base", boxId)));
        auto filleted = engine.recompute(doc);
        if (!filleted) {
            std::fprintf(stderr, "fillet recompute failed: %s\n",
                         filleted.error().message.c_str());
            return 1;
        }
        doc = filleted.value().first;
        const auto obj = doc.find(filletId);
        if (obj != nullptr && obj->output() != nullptr) {
            shown = filletId;
            std::printf("fillet elements: %zu\n", obj->output()->map.allNames().size());
        } else {
            std::fprintf(stderr, "fillet produced nothing: %s\n",
                         obj != nullptr ? obj->error().message.c_str() : "no object");
        }
    }

    render::MeshCache meshes(blobs);
    auto gpu = backend.handle();
    render::SceneBuilder scene(meshes, *gpu.resources);

    std::vector<render::Placement> placements(1);
    placements[0].object = shown;
    static constexpr float identity[12]{1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    std::copy(identity, identity + 12, placements[0].transform);
    if (auto r = scene.update(doc, placements); !r) {
        std::fprintf(stderr, "scene failed: %s\n", r.error().message.c_str());
        return 1;
    }

    render::CameraController camera;
    camera.setHomogeneousDepth(backend.homogeneousDepth());
    camera.fit(scene.bounds(), config.viewport);
    scene.setViewport(config.viewport);
    scene.setCamera(camera.matrices(config.viewport));

    // The dominant shades, not just how many. Counting alone is invariant under a colour change --
    // 5 shades before and 5 after says nothing about whether the tint arrived, which is exactly the
    // "assertion that agrees with itself" this project keeps tripping over.
    using Sampled = std::pair<std::size_t, std::vector<std::uint32_t>>;
    const auto sample = [&](const char* label) -> Sampled {
        gpu.frames->submit(scene.frame());
        auto pixels = backend.captureFrame();
        if (!pixels) {
            std::fprintf(stderr, "capture failed: %s\n", pixels.error().message.c_str());
            return Sampled{0, {}};
        }
        // Counted rather than eyeballed. A correctly tinted body has a handful of shades — one per
        // face, times the lighting. A dithered one has dozens, because adjacent fragments on the
        // same face disagree.
        std::map<std::uint32_t, std::size_t> histogram;
        const auto& px = pixels.value();
        for (std::size_t i = 0; i + 3 < px.size(); i += 4) {
            const std::uint32_t key = (std::uint32_t(px[i]) << 16) | (std::uint32_t(px[i + 1]) << 8)
                                      | std::uint32_t(px[i + 2]);
            ++histogram[key];
        }
        // Only shades that cover real area, so anti-aliasing along silhouettes is not counted as
        // dithering. A dither covers large areas in both colours; an edge covers a thin line.
        std::size_t significant = 0;
        for (const auto& [colour, count] : histogram) {
            if (count > 200) ++significant;
        }
        std::vector<std::pair<std::size_t, std::uint32_t>> ranked;
        for (const auto& [colour, count] : histogram) ranked.emplace_back(count, colour);
        std::sort(ranked.rbegin(), ranked.rend());

        std::printf("%-22s %4zu shades, %3zu over 200px   top:", label, histogram.size(),
                    significant);
        for (std::size_t i = 0; i < ranked.size() && i < 4; ++i) {
            std::printf(" #%06x(%zu)", ranked[i].second, ranked[i].first);
        }
        std::printf("\n");

        std::vector<std::uint32_t> top;
        for (std::size_t i = 0; i < ranked.size() && i < 4; ++i) top.push_back(ranked[i].second);
        return Sampled{significant, top};
    };

    const auto [plain, plainTop] = sample("no highlight:");

    // Highlight EVERY element, which is what selecting a body does. If the lookup works, the whole
    // body becomes one tint and the shade count should not explode.
    for (std::uint32_t e = 0; e < 256; ++e) scene.setHighlight(e, render::Highlight::Selected);
    const auto [highlighted, litTop] = sample("body highlighted:");

    // ── GPU PICKING, verified end to end for the first time ──────────────────────────────
    //
    // The element-id fix (ids were 255x too large, because a_color1 is UNNORMALISED and the shader
    // multiplied by 255 anyway) landed without ever being checked through the real picker: every
    // picking test drives the scripted null picker by design, and the shell never drove the GPU one.
    // So "picking works" has been an untested claim. This is the test.
    gpu.frames->submit(scene.frame());

    const std::uint32_t cx = config.viewport.width / 2;
    const std::uint32_t cy = config.viewport.height / 2;
    const auto hit = gpu.picker->pick(scene.frame(), cx, cy);
    std::printf("pick at centre (%u,%u): valid=%d element=%u depth=%.3f\n", cx, cy,
                hit.valid ? 1 : 0, hit.element, hit.depth);

    bool pickOk = true;
    if (!hit.valid) {
        std::fprintf(stderr, "FAIL: nothing picked at the centre of the screen, where the body is. "
                             "GPU picking does not work.\n");
        pickOk = false;
    } else if (hit.element >= 256) {
        // The 255x bug's signature. A body with ~100 elements cannot legitimately report a slot in
        // the thousands, and that is exactly what an unnormalised attribute scaled by 255 produces.
        std::fprintf(stderr, "FAIL: picked element %u, which is far outside this body's element "
                             "range — the id scaling is wrong again.\n", hit.element);
        pickOk = false;
    } else if (!scene.resolve(hit)) {
        std::fprintf(stderr, "FAIL: picked element %u does not resolve to a name. A pick that "
                             "cannot be named cannot be selected.\n", hit.element);
        pickOk = false;
    } else {
        std::printf("PASS  the centre pick resolved to element %u\n", hit.element);
    }

    // ── THE APERTURE, which is what touch selection actually calls ───────────────────────
    //
    // Added because the iPad reported "0 candidates, miss" on every tap while the model was plainly
    // on screen, and the single-pixel pick above passes. Those two facts cannot both be about the
    // same code unless the aperture read is broken — so this measures it here, on a Mac, where the
    // answer arrives in seconds instead of a build-install-tap cycle.
    std::vector<render::IPicker::ApertureHit> aperture;
    gpu.picker->pickAperture(scene.frame(), cx, cy, 44, aperture);
    std::printf("aperture r=44 at centre: %zu candidate(s)\n", aperture.size());
    if (aperture.empty()) {
        std::fprintf(stderr, "FAIL: the aperture found nothing where the single-pixel pick found "
                             "an element. Touch selection cannot work.\n");
        pickOk = false;
    } else {
        // The centre pixel's element must be among them: an aperture that returns a DIFFERENT set
        // from the pick it contains is worse than one that returns nothing.
        const bool containsCentre =
            std::any_of(aperture.begin(), aperture.end(),
                        [&](const render::IPicker::ApertureHit& h) { return h.element == hit.element; });
        std::printf("%s  the aperture contains the centre pick\n", containsCentre ? "PASS" : "FAIL");
        if (!containsCentre) pickOk = false;
    }

    // A tap NEAR the silhouette, which is the case touch exists for: 20 pixels outside the body,
    // where a one-pixel test misses and a fingertip should not.
    std::vector<render::IPicker::ApertureHit> nearMiss;
    gpu.picker->pickAperture(scene.frame(), 20, cy, 44, nearMiss);
    std::printf("aperture r=44 at the left edge (20,%u): %zu candidate(s)\n", cy, nearMiss.size());

    // And empty space must NOT pick. A picker that always hits is as useless as one that never
    // does, and it is the easier mistake to miss.
    const auto miss = gpu.picker->pick(scene.frame(), 4, 4);
    std::printf("pick at corner (4,4): valid=%d\n", miss.valid ? 1 : 0);
    if (miss.valid) {
        std::fprintf(stderr, "FAIL: picked something in empty space at the corner.\n");
        pickOk = false;
    }

    backend.shutdown();

    std::printf("\n");
    if (!pickOk) return 1;

    // FIRST: did anything change at all? A clean tint and a no-op both keep the shade COUNT the
    // same, so the count can only be read after establishing that the colours moved.
    if (plainTop == litTop) {
        std::fprintf(stderr,
                     "FAIL: highlighting changed no pixels. The tint is not reaching the shader on "
                     "this path at all — the lookup, the uniform, or the sampler is the fault, not "
                     "the per-face uniformity.\n");
        return 1;
    }
    std::printf("highlight DID change the image, so the lookup reaches the shader.\n");

    if (highlighted > plain * 3 + 4) {
        std::fprintf(stderr,
                     "FAIL: highlighting multiplied the shade count %zu -> %zu. Adjacent fragments "
                     "on one face are reading different highlight values, so v_ids.x is NOT "
                     "constant per face.\n",
                     plain, highlighted);
        return 1;
    }
    std::printf("PASS  highlighting kept the shade count sane (%zu -> %zu): the per-element lookup "
                "is uniform across each face\n", plain, highlighted);
    return 0;
}
