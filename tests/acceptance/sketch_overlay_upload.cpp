// The sketch overlay's vertices must match the layout the buffer is created with.
//
// # What went wrong
//
// Reported as "multiple lines appear when drawing": clicking the first point of a line and moving
// the pointer drew a fan of dozens of lines radiating from nowhere, growing with every mouse move,
// while the status bar correctly read "0 curves". Nothing was wrong with the geometry — the sketch
// really was empty, and the preview really was one line.
//
// The edge vertex layout is position PLUS the element id: 16 bytes. The sketch overlay and preview
// are the only edge geometry described as bare xyz floats, because they are generated rather than
// tessellated, and they were handed to the buffer as 12-byte vertices with a count of `floats / 3`.
// So the draw asked for N vertices of 16 bytes from a buffer holding N vertices of 12 — every
// vertex after the first read from the wrong offset, and the last third of the draw ran past the
// data into whatever the previous, longer upload had left behind. Hence a fan, and hence a fan that
// accumulated.
//
// # What is asserted
//
// The bytes uploaded and the vertex count drawn have to agree, which is the invariant that was
// violated. The seam is now typed (`std::span<const EdgeVertex>`), so the compiler refuses the
// mismatch — this test is the belt to that braces, and it fails loudly if anyone reintroduces a
// float-shaped overload.

#include "cad/recompute/DdcCache.h"
#include "cad/render/NullBackend.h"
#include "cad/render/Scene.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>
#include <vector>

using namespace cad;

namespace {

/// The key `SceneBuilder` uses for the preview buffer. Duplicated from Scene.cpp deliberately: it
/// is an internal detail, and a test that read it from the source of truth could not notice the
/// source of truth changing.
constexpr std::uint64_t kSketchPreviewKey = 0x5E7C4000000002ull;

}  // namespace

TEST_CASE("the sketch preview uploads as many bytes as it draws vertices", "[sketch][render]") {
    recompute::MemoryBlobStore blobs;
    render::MeshCache meshes{blobs};
    render::NullBackend backend;
    render::SceneBuilder scene(meshes, backend.resources);

    // Three points of a dashed preview, as the drawing layer produces them: xyz triples.
    const std::vector<float> xyz{0, 0, 0, 10, 0, 0, 20, 5, 0};
    scene.setSketchPreview(xyz, /*revision=*/1);

    const auto& frame = scene.frame();
    REQUIRE_FALSE(frame.edgeBatches.empty());
    const auto& preview = frame.edgeBatches.back();
    REQUIRE(preview.vertexCount == 3);

    // The invariant. 3 vertices at 16 bytes each; the bug uploaded 36 and drew 48.
    CHECK(backend.resources.dynamicEdgeBytes(kSketchPreviewKey)
          == preview.vertexCount * sizeof(render::EdgeVertex));
}

TEST_CASE("a shorter preview draws fewer vertices than the one before it", "[sketch][render]") {
    // The accumulation half. Every mouse move replaces the preview, and a move back towards the
    // start makes it SHORTER — at which point a stale count keeps drawing the tail of the previous
    // one. That is what turned a wrong-stride bug into a growing fan rather than a single wrong
    // line.
    recompute::MemoryBlobStore blobs;
    render::MeshCache meshes{blobs};
    render::NullBackend backend;
    render::SceneBuilder scene(meshes, backend.resources);

    const std::vector<float> longPreview{0, 0, 0, 10, 0, 0, 20, 0, 0, 30, 0, 0, 40, 0, 0};
    scene.setSketchPreview(longPreview, 1);
    REQUIRE(scene.frame().edgeBatches.back().vertexCount == 5);

    const std::vector<float> shortPreview{0, 0, 0, 4, 0, 0};
    scene.setSketchPreview(shortPreview, 2);
    const auto& preview = scene.frame().edgeBatches.back();
    CHECK(preview.vertexCount == 2);
    CHECK(backend.resources.dynamicEdgeBytes(kSketchPreviewKey)
          == preview.vertexCount * sizeof(render::EdgeVertex));
}

TEST_CASE("an empty preview draws nothing at all", "[sketch][render]") {
    // Escape, or finishing a segment, clears the preview. If clearing left the batch behind, the
    // last rubber band would stay on screen as a line the user cannot select or delete.
    recompute::MemoryBlobStore blobs;
    render::MeshCache meshes{blobs};
    render::NullBackend backend;
    render::SceneBuilder scene(meshes, backend.resources);

    scene.setSketchPreview(std::vector<float>{0, 0, 0, 10, 0, 0}, 1);
    REQUIRE_FALSE(scene.frame().edgeBatches.empty());

    scene.setSketchPreview({}, 0);
    CHECK(scene.frame().edgeBatches.empty());
}
