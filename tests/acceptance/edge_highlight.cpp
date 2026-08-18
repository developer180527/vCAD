// A highlighted EDGE has to be drawn in its highlight colour, and that needs its own draw call.
//
// This is the second half of the bug that made selection invisible. `u_highlight` was a uniform,
// and a uniform is per DRAW CALL while one call covers a whole mesh across every placement — so no
// value could mean "this edge and not the other eleven". Faces were fixed with a per-element lookup
// texture; edges cannot use it because the edge vertex stream carries positions only and `vs_edge`
// writes no element id.
//
// The cheaper answer, and the one taken: draw the highlighted edge AGAIN over its own vertex
// sub-range, in the highlight colour. `RenderMesh::edges` already carries
// `EdgeRange{vertexOffset, vertexCount, element}` and `EdgeBatch` already has a colour and a vertex
// sub-range, so it needs no change to the mesh format, the tessellator, or the content hash.

#include "cad/document/Document.h"
#include "cad/features/Builtins.h"
#include "cad/recompute/Engine.h"
#include "cad/recompute/DdcCache.h"
#include "cad/render/Tessellate.h"
#include "cad/render/NullBackend.h"
#include "cad/render/Camera.h"
#include "cad/render/Scene.h"
#include "cad/units/Units.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <memory>
#include <vector>

using namespace cad;

namespace {

/// A one-box scene, built through the real recompute so the element ids and edge ranges are the
/// ones the application actually produces.
struct Scene {
    recompute::MemoryBlobStore blobs;
    render::MeshCache meshes{blobs};
    render::NullBackend backend;
    std::unique_ptr<render::SceneBuilder> scene;
    document::Document doc;

    Scene() {
        auto [withBox, id] = doc.add("Box");
        doc = withBox;
        doc = doc.replace(std::make_shared<const document::ObjectData>(
            doc.find(id)->withProperty("dx", units::millimetres(40))
                .withProperty("dy", units::millimetres(30))
                .withProperty("dz", units::millimetres(20))));

        const auto registry = features::builtins();
        recompute::MemoryCache cache;
        recompute::Engine engine(registry, cache);
        auto computed = engine.recompute(doc);
        REQUIRE(computed);
        doc = computed.value().first;

        scene = std::make_unique<render::SceneBuilder>(meshes, backend.resources);

        std::vector<render::Placement> placements(1);
        placements[0].object = id;
        static constexpr float identity[12]{1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
        std::copy(identity, identity + 12, placements[0].transform);
        REQUIRE(scene->update(doc, placements));

        // Culling assigns the draw ranges an overlay copies, so a camera is required before the
        // overlays can exist at all.
        render::Viewport viewport{1280, 800, 1.0f};
        scene->setViewport(viewport);
        render::CameraController camera;
        camera.fit(scene->bounds(), viewport);
        scene->setCamera(camera.matrices(viewport));
    }
};

}  // namespace

TEST_CASE("highlighting an edge adds a draw call for that edge alone", "[render][highlight]") {
    Scene s;
    const std::size_t base = s.scene->frame().edgeBatches.size();
    REQUIRE(base > 0);

    // Which element is an edge is a property of the mesh, not something to hardcode. Highlight
    // each in turn until the overlay appears; the FIRST one that produces an extra batch is an
    // edge, and if none does then the feature does not work.
    std::size_t withOverlay = base;
    for (std::uint32_t element = 0; element < 64 && withOverlay == base; ++element) {
        s.scene->clearHighlights();
        s.scene->setHighlight(element, render::Highlight::Selected);
        withOverlay = s.scene->frame().edgeBatches.size();
    }

    INFO("base edge batches: " << base << ", after highlighting: " << withOverlay);
    REQUIRE(withOverlay > base);

    // The overlay must be a SUB-RANGE, not the whole mesh's edges again — otherwise every edge of
    // the body lights up and the highlight says nothing about what was selected.
    const auto& batches = s.scene->frame().edgeBatches;
    const auto& overlay = batches[base];
    const auto& first = batches[0];
    CHECK(overlay.vertexCount > 0);
    CHECK(overlay.vertexCount < first.vertexCount);

    // And it must be a different colour from the base edges, or it is invisible.
    const bool differs = overlay.colour[0] != first.colour[0]
                         || overlay.colour[1] != first.colour[1]
                         || overlay.colour[2] != first.colour[2];
    CHECK(differs);

    // It draws the same instances as the base batch: a highlighted edge of a mesh placed fifty
    // times is highlighted in all fifty, because an element id identifies geometry in the
    // document rather than one thing on screen.
    CHECK(overlay.instances == first.instances);
    CHECK_FALSE(overlay.ranges.empty());
}

TEST_CASE("clearing highlights removes the overlays", "[render][highlight]") {
    Scene s;
    const std::size_t base = s.scene->frame().edgeBatches.size();

    for (std::uint32_t element = 0; element < 64; ++element) {
        s.scene->setHighlight(element, render::Highlight::Selected);
    }
    REQUIRE(s.scene->frame().edgeBatches.size() > base);

    s.scene->clearHighlights();
    CHECK(s.scene->frame().edgeBatches.size() == base);
}

TEST_CASE("a camera move keeps the overlays alive", "[render][highlight]") {
    // The trap in this design: an overlay copies its base batch's draw ranges, and culling rebuilds
    // those on every camera change. If the overlays were not rebuilt too, a selection would vanish
    // the moment the user orbited — which is worse than never highlighting, because it looks random.
    Scene s;
    const std::size_t base = s.scene->frame().edgeBatches.size();

    std::size_t withOverlay = base;
    for (std::uint32_t element = 0; element < 64 && withOverlay == base; ++element) {
        s.scene->clearHighlights();
        s.scene->setHighlight(element, render::Highlight::Selected);
        withOverlay = s.scene->frame().edgeBatches.size();
    }
    REQUIRE(withOverlay > base);

    render::Viewport viewport{1280, 800, 1.0f};
    render::CameraController camera;
    camera.fit(s.scene->bounds(), viewport);
    camera.orbit(30.0f, 20.0f);
    s.scene->setCamera(camera.matrices(viewport));

    CHECK(s.scene->frame().edgeBatches.size() == withOverlay);
}

TEST_CASE("the sketch overlay draws on top and nothing else does", "[render][sketch]") {
    Scene s;

    // Two segments in world space, as Controller::sketchOverlayVertices produces them.
    const float lines[]{0.0f, 0.0f, 0.0f, 40.0f, 0.0f, 0.0f,
                        40.0f, 0.0f, 0.0f, 40.0f, 25.0f, 0.0f};
    const std::size_t before = s.scene->frame().edgeBatches.size();
    s.scene->setSketchOverlay(lines, 1u);

    const auto& batches = s.scene->frame().edgeBatches;
    REQUIRE(batches.size() == before + 1);

    // LAST, so it draws over the model and over any highlight: it is the thing being edited.
    const render::EdgeBatch& overlay = batches.back();
    CHECK(overlay.onTop);
    CHECK(overlay.vertexCount == 4);

    // And ONLY it. Model edges hidden behind the model should stay hidden — that is what makes a
    // shaded view readable, and an on-top flag leaking onto them turns the part into a wireframe.
    for (std::size_t i = 0; i < batches.size() - 1; ++i) {
        CHECK_FALSE(batches[i].onTop);
    }

    // Clearing removes it rather than leaving a stale sketch over the model after Finish Sketch.
    s.scene->setSketchOverlay({}, 2u);
    CHECK(s.scene->frame().edgeBatches.size() == before);
}

TEST_CASE("the sketch profile shades only when the curves close", "[render][sketch]") {
    Scene s;
    const std::size_t before = s.scene->frame().batches.size();

    // A triangle: three vertices, one face.
    const render::CadVertex verts[]{
        {{0, 0, 0}, {0, 0, 1}, 0}, {{10, 0, 0}, {0, 0, 1}, 0}, {{0, 10, 0}, {0, 0, 1}, 0}};
    const std::uint32_t idx[]{0, 1, 2};

    s.scene->setSketchProfile(verts, idx, 1u);
    const auto& batches = s.scene->frame().batches;
    REQUIRE(batches.size() == before + 1);

    const render::Batch& profile = batches.back();
    CHECK(profile.indexCount == 3);
    // Blended, double-sided and on top. On top is the load-bearing one: the body being sketched
    // against usually sits between the sketch plane and the camera, so a depth-tested fill is
    // hidden inside the solid and the user sees edges enclosing nothing.
    CHECK(profile.blended);
    CHECK(profile.doubleSided);
    CHECK(profile.onTop);

    // And ONLY it. A model batch drawing on top would turn the part into a wireframe.
    for (std::size_t i = 0; i < batches.size() - 1; ++i) {
        CHECK_FALSE(batches[i].blended);
        CHECK_FALSE(batches[i].onTop);
    }

    // An OPEN profile clears the shading. This is the signal: shading appears exactly when the
    // curves close, so its absence is the answer to "why will this not extrude".
    s.scene->setSketchProfile({}, {}, 2u);
    CHECK(s.scene->frame().batches.size() == before);
}

TEST_CASE("culling does not drop the sketch profile", "[render][sketch]") {
    Scene s;
    const std::size_t before = s.scene->frame().batches.size();
    const render::CadVertex verts[]{
        {{0, 0, 0}, {0, 0, 1}, 0}, {{10, 0, 0}, {0, 0, 1}, 0}, {{0, 10, 0}, {0, 0, 1}, 0}};
    const std::uint32_t idx[]{0, 1, 2};
    s.scene->setSketchProfile(verts, idx, 1u);
    REQUIRE(s.scene->frame().batches.size() == before + 1);

    // Culling rewrites frame_.batches from its own list, which is exactly how an appended batch
    // gets silently dropped — on the next camera move, not on the frame that added it.
    render::CameraController camera;
    camera.orbit(20.0f, 15.0f);
    s.scene->setCamera(camera.matrices(render::Viewport{1280, 800, 1.0f}));

    CHECK(s.scene->frame().batches.size() == before + 1);
    CHECK(s.scene->frame().batches.back().blended);
}
