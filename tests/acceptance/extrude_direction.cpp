/// An extrude goes along its profile's own normal, not along a global axis.
///
/// # Why this test asserts a coordinate
///
/// `sketch_plane.cpp` already covers sketch-on-a-face, and every assertion in it is about the
/// DECLARATION: that `placement().kind` is `Face`, that the face string round-trips, that
/// `needsResolution()` is true. All of those can pass while the geometry is built somewhere else
/// entirely, because the test and the code share the same assumption and so agree with each other.
///
/// This file asserts where the solid ENDED UP. It is the only kind of check that can tell the
/// difference between a sketch that is placed on a face and one that merely says it is.
///
/// # Why the +X face specifically
///
/// A box's faces are axis-aligned, so a sketch on its top face extrudes along Z whether the
/// direction comes from the face normal or from the stored XY plane index — the bug and the fix
/// agree there, and a test on the top face would pass either way. The +X face is chosen because the
/// two answers DISAGREE: the correct one grows the solid along X, and the plane-index one grows it
/// along Z.

#include "cad/document/Document.h"
#include "cad/features/Builtins.h"
#include "cad/kernel/Shape.h"
#include "cad/recompute/Engine.h"
#include "cad/recompute/DdcCache.h"
#include "cad/sketch/Sketch.h"
#include "cad/units/Units.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <memory>
#include <string>

using namespace cad;

namespace {

/// Recomputes and returns the document, failing the test if anything refused.
document::Document recomputed(const document::Document& doc) {
    const auto registry = features::builtins();
    recompute::MemoryCache cache;
    recompute::Engine engine(registry, cache);
    auto computed = engine.recompute(doc);
    REQUIRE(computed);
    return computed.value().first;
}

/// The name of the box face whose outward normal points along +X.
///
/// Found by measuring rather than by guessing the string: the naming layer's spelling of a face is
/// its own business, and a test that hard-coded "Box#1/face:X+" would break for a reason that has
/// nothing to do with what it is checking.
std::string faceNamePointingAlongX(const document::ObjectData& box) {
    REQUIRE(box.output() != nullptr);
    for (const auto& name : box.output()->map.allNames()) {
        const auto shape = box.output()->map.resolve(name);
        if (!shape) continue;
        const auto plane = kernel::planeOf(*shape);
        if (!plane) continue;   // not a planar face: an edge or a vertex
        const auto& f = plane.value();
        // u x v is the face normal. |x| dominant means this face looks along X.
        const double nx = f.u[1] * f.v[2] - f.u[2] * f.v[1];
        const double ny = f.u[2] * f.v[0] - f.u[0] * f.v[2];
        const double nz = f.u[0] * f.v[1] - f.u[1] * f.v[0];
        if (std::abs(nx) > 0.9 && std::abs(ny) < 0.1 && std::abs(nz) < 0.1) {
            // The +X one, not the -X one, so the expected growth direction is unambiguous.
            if (f.origin[0] > 1.0) return name.toString();
        }
    }
    return {};
}

}  // namespace

TEST_CASE("a sketch on a side face extrudes along that face's normal", "[sketch][extrude]") {
    document::Document doc;

    auto [withBox, boxId] = doc.add("Box");
    doc = withBox;
    doc = doc.replace(std::make_shared<const document::ObjectData>(
        doc.find(boxId)->withProperty("dx", units::millimetres(40))
            .withProperty("dy", units::millimetres(30))
            .withProperty("dz", units::millimetres(20))));
    doc = recomputed(doc);

    const std::string faceName = faceNamePointingAlongX(*doc.find(boxId));
    REQUIRE_FALSE(faceName.empty());

    // A closed square profile, drawn in the sketch's own 2D coordinates.
    sketch::Sketch profile;
    sketch::SketchPlane placement;
    placement.kind = sketch::SketchPlane::Kind::Face;
    placement.face = faceName;
    profile.setPlacement(placement);
    profile.addLine(2.0, 2.0, 8.0, 2.0);
    profile.addLine(8.0, 2.0, 8.0, 8.0);
    profile.addLine(8.0, 8.0, 2.0, 8.0);
    profile.addLine(2.0, 8.0, 2.0, 2.0);

    auto [withSketch, sketchId] = doc.add("Sketch");
    doc = withSketch;
    doc = doc.replace(std::make_shared<const document::ObjectData>(
        doc.find(sketchId)->withProperty("sketch", profile.serialize())
            .withProperty("body", boxId)));
    doc = recomputed(doc);
    REQUIRE(doc.find(sketchId)->output() != nullptr);

    auto [withExtrude, extrudeId] = doc.add("Extrude");
    doc = withExtrude;
    doc = doc.replace(std::make_shared<const document::ObjectData>(
        doc.find(extrudeId)->withProperty("distance", units::millimetres(5))
            .withProperty("profile", sketchId)));
    doc = recomputed(doc);

    const auto extruded = doc.find(extrudeId);
    REQUIRE(extruded != nullptr);
    REQUIRE(extruded->output() != nullptr);

    // The whole point, as a COORDINATE. The profile sits on the +X face at x == 40, so a correct
    // extrude carries the solid outward along +X and puts its centre of mass at x == 42.5. The
    // plane-index path extrudes along Z instead, which leaves the centroid at x == 40.
    const auto centre = extruded->output()->shape.measure();
    CHECK_THAT(centre.cx, Catch::Matchers::WithinAbs(42.5, 0.2));

    // A 6x6 profile 5 mm thick. Volume catches a boolean or a sweep that silently did nothing far
    // more reliably than a face count does.
    CHECK_THAT(extruded->output()->shape.volume(), Catch::Matchers::WithinRel(180.0, 0.02));
}

TEST_CASE("a sketch on a global plane still extrudes the way it always did", "[sketch][extrude]") {
    // The direction now comes from the profile for EVERY sketch, not only face-placed ones, so the
    // global planes changed code path even though they did not change meaning. Their sign is the
    // thing most easily flipped by measuring a normal instead of hard-coding an axis, and nothing
    // else in the suite pins it down -- the older tests assert that an extrude produced a solid,
    // not which side of the sketch it grew towards.
    struct Case {
        sketch::Plane plane;
        int axis;           ///< 0=x, 1=y, 2=z: the one the solid should grow along
    };
    const auto sample = GENERATE(Case{sketch::Plane::XY, 2}, Case{sketch::Plane::XZ, 1},
                                 Case{sketch::Plane::YZ, 0});

    document::Document doc;

    sketch::Sketch profile;
    sketch::SketchPlane placement;
    placement.kind = sketch::SketchPlane::Kind::Global;
    placement.global = sample.plane;
    profile.setPlacement(placement);
    profile.addLine(0.0, 0.0, 6.0, 0.0);
    profile.addLine(6.0, 0.0, 6.0, 6.0);
    profile.addLine(6.0, 6.0, 0.0, 6.0);
    profile.addLine(0.0, 6.0, 0.0, 0.0);

    auto [withSketch, sketchId] = doc.add("Sketch");
    doc = withSketch;
    doc = doc.replace(std::make_shared<const document::ObjectData>(
        doc.find(sketchId)->withProperty("sketch", profile.serialize())));

    // The plane index is set exactly as Controller::addExtrude sets it for a global sketch, because
    // that index is what pins the historical direction for these three planes.
    auto [withExtrude, extrudeId] = doc.add("Extrude");
    doc = withExtrude;
    doc = doc.replace(std::make_shared<const document::ObjectData>(
        doc.find(extrudeId)->withProperty("distance", units::millimetres(4))
            .withProperty("profile", sketchId)
            .withProperty("plane", static_cast<std::int64_t>(sample.plane))));
    doc = recomputed(doc);

    const auto extruded = doc.find(extrudeId);
    REQUIRE(extruded != nullptr);
    REQUIRE(extruded->output() != nullptr);

    // A real solid, and its centre sits half the extrude distance off the sketch plane -- on the
    // POSITIVE side, which is the direction these three planes have always grown towards.
    CHECK_THAT(extruded->output()->shape.volume(), Catch::Matchers::WithinRel(144.0, 0.02));
    const auto centre = extruded->output()->shape.measure();
    const double along = sample.axis == 0 ? centre.cx : sample.axis == 1 ? centre.cy : centre.cz;
    CHECK_THAT(along, Catch::Matchers::WithinAbs(2.0, 0.05));
}
