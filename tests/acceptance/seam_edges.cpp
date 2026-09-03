// A seam is not an edge of the part.
//
// # What was wrong
//
// Every cylinder in the application was drawn with a black line down its side, and every dome with
// one across it. Reported as "the renderer does not seem okay — I can see edges in a supposedly
// smooth surface", which is exactly right.
//
// The line is the SEAM: the edge where a closed surface's parameterisation wraps around, u = 2*pi
// meeting u = 0. It is a real, non-degenerate `TopoDS_Edge`, so the tessellator's degeneracy test
// never caught it — that test skips a collapsed POLE, which is a different thing with no length to
// draw at all.
//
// # Why it is not a cosmetic complaint
//
// The seam describes how the surface is written down, not anything about the part. It is placed
// wherever the modelling operation happened to start the parameterisation, so two cylinders that
// are geometrically identical can carry it in different places, and a user rotating the model
// watches it sit still while the silhouette moves. No production CAD draws it. Drawing it makes a
// smooth face look like it has a crease in it, which is the one thing a shaded CAD view has to get
// right.
//
// # What is asserted
//
// Counted rather than looked at. A cylinder has three edges — two circles and a seam — so the
// question is whether two get drawn or three. A box has twelve and no seams, which is the control:
// a fix that simply dropped edges would pass the first assertion and fail that one.

#include "cad/features/Builtins.h"
#include "cad/recompute/Engine.h"
#include "cad/render/Tessellate.h"
#include "cad/units/Units.h"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>

using namespace cad;

namespace {

/// One primitive, computed, ready to tessellate.
///
/// Its dimensions come from the feature's own declared defaults rather than from numbers written
/// here, so this cannot drift from what the application actually builds when someone presses the
/// button — and a primitive that gained a property would not silently start failing to compute.
document::Output computed(const std::string& type) {
    const auto registry = features::builtins();
    const auto* feature = registry.find(type);
    REQUIRE(feature != nullptr);

    document::Document doc;
    auto [added, id] = doc.add(type);
    auto seeded = *added.find(id);
    for (const auto& value : feature->inputs.values) {
        seeded = seeded.withProperty(value.name, units::millimetres(value.base));
    }
    doc = added.replace(std::make_shared<const document::ObjectData>(std::move(seeded)));

    recompute::MemoryCache cache;
    recompute::Engine engine(registry, cache);
    auto result = engine.recompute(doc);
    REQUIRE(result);

    const auto object = result.value().first.find(id);
    REQUIRE(object);
    REQUIRE(object->output() != nullptr);
    return *object->output();
}

std::size_t drawnEdgesOf(const document::Output& output) {
    auto mesh = render::tessellate(output, render::TessellationSettings{});
    REQUIRE(mesh);
    return mesh.value()->edges.size();
}

}  // namespace

TEST_CASE("a cylinder's seam is not drawn", "[render][seam]") {
    // Three edges exist on a cylinder: the top circle, the bottom circle, and the seam running up
    // the side. Two of them are real. The third is where the surface's parameterisation wraps, and
    // it is the black line users were seeing down the middle of every round face.
    CHECK(drawnEdgesOf(computed("Cylinder")) == 2);
}

TEST_CASE("a box still draws all twelve edges", "[render][seam]") {
    // The control, and the reason this file is two tests rather than one. A "fix" that dropped
    // edges too eagerly — every closed curve, say, or every edge shared by two faces — would pass
    // the cylinder assertion and quietly stop drawing the model.
    CHECK(drawnEdgesOf(computed("Box")) == 12);
}

TEST_CASE("dropping the seam does not drop its face", "[render][seam]") {
    // The seam bounds the cylindrical face. Removing it from the DRAWN set must not remove it from
    // the tessellation, or the side of every cylinder disappears — which would be a far louder bug
    // than the one being fixed, and one a test counting edges alone would not notice.
    const auto output = computed("Cylinder");
    auto mesh = render::tessellate(output, render::TessellationSettings{});
    REQUIRE(mesh);

    CHECK(mesh.value()->faces.size() == 3);        // side, top, bottom
    CHECK_FALSE(mesh.value()->vertices.empty());
    CHECK_FALSE(mesh.value()->indices.empty());
}

TEST_CASE("a curve is tessellated finely enough to read as a curve", "[render][seam]") {
    // A floor on quality, not an exact count -- the number depends on OCCT's meshing and pinning it
    // would fail on an upgrade that made the mesh BETTER, which is the wrong way round.
    //
    // The default was 0.35 rad, 20 degrees, about 18 segments around a circle. That is coarse
    // enough to see without looking for it, and it was: a fillet rounding a cylinder into a dome
    // came out with a visibly straight-edged silhouette and was reported as the renderer being
    // broken. 0.20 rad is 11.5 degrees and about 31 segments.
    //
    // Asserted through the SETTINGS rather than by counting triangles at the default, so this says
    // what it means: the default must stay at least this fine.
    CHECK(render::TessellationSettings{}.angularDeflection <= 0.20);

    // And the mesh really does get finer when the setting does -- the incremental mesher keeps an
    // existing triangulation it considers adequate, so a quality setting can silently do nothing.
    // That has happened here before; see the BRepTools::Clean note in Tessellate.cpp.
    const auto output = computed("Cylinder");
    render::TessellationSettings coarse;
    coarse.angularDeflection = 0.35;
    render::TessellationSettings fine;
    fine.angularDeflection = 0.10;

    auto coarseMesh = render::tessellate(output, coarse);
    auto fineMesh = render::tessellate(output, fine);
    REQUIRE(coarseMesh);
    REQUIRE(fineMesh);
    CHECK(fineMesh.value()->indices.size() > coarseMesh.value()->indices.size());
}
