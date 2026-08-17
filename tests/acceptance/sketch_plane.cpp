// Where a sketch is drawn, and whether that survives being written to a file and read back.
//
// `Sketch::serialize`/`deserialize` is a SAVED FILE FORMAT — it is what goes into a `.vpart`. So the
// interesting cases are not "does a new field round-trip" but the two compatibility directions:
// a file written before this field existed must still load and still mean what it meant, and a
// file written with it must not become unreadable to a build that predates it.
//
// Step 1a of the sketch-plane work. See docs/design/SKETCH_PLANE_PROGRESS.md.

#include "cad/document/Document.h"
#include "cad/features/Builtins.h"
#include "cad/kernel/Primitives.h"
#include "cad/recompute/Engine.h"
#include "cad/units/Units.h"
#include "cad/kernel/Shape.h"
#include "cad/sketch/Sketch.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

#include <string>

using namespace cad::sketch;

TEST_CASE("a global-plane sketch round-trips unchanged", "[sketch][plane]") {
    Sketch sketch(Plane::XZ);
    sketch.addLine(0, 0, 10, 0);
    sketch.addCircle(5, 5, 2);

    const auto parsed = Sketch::deserialize(sketch.serialize());
    REQUIRE(parsed);
    CHECK(parsed.value().plane() == Plane::XZ);
    CHECK(parsed.value().placement().kind == SketchPlane::Kind::Global);
    CHECK(parsed.value().geometry().size() == 2);
    CHECK_FALSE(parsed.value().needsResolution());
}

TEST_CASE("a face-placed sketch round-trips its reference", "[sketch][plane]") {
    SketchPlane placement;
    placement.kind = SketchPlane::Kind::Face;
    placement.global = Plane::XY;
    // A real element name, with the spaces and punctuation one actually contains — the field is
    // written last on its line precisely so this survives.
    placement.face = "Box#1/face:Z+ from Extrude#3";

    Sketch sketch(placement);
    sketch.addLine(0, 0, 10, 0);

    const auto parsed = Sketch::deserialize(sketch.serialize());
    REQUIRE(parsed);
    CHECK(parsed.value().placement().kind == SketchPlane::Kind::Face);
    CHECK(parsed.value().placement().face == "Box#1/face:Z+ from Extrude#3");
    CHECK(parsed.value().needsResolution());
}

TEST_CASE("a file written before placements existed still loads", "[sketch][plane]") {
    // Byte-for-byte what an older build wrote: a bare `plane` line and no placement at all. This
    // is the direction that matters most — every `.vpart` saved to date looks like this, and
    // "still loads" is not enough on its own. It must still MEAN what it meant, which is Global.
    const std::string old =
        "sketch 1\n"
        "plane YZ\n"
        "g 1 line 0 0 0 10 0\n";

    const auto parsed = Sketch::deserialize(old);
    REQUIRE(parsed);
    CHECK(parsed.value().plane() == Plane::YZ);
    CHECK(parsed.value().placement().kind == SketchPlane::Kind::Global);
    CHECK_FALSE(parsed.value().needsResolution());
    CHECK(parsed.value().geometry().size() == 1);
}

TEST_CASE("a placement is written as ADDITIONAL lines, not by changing the old one",
          "[sketch][plane]") {
    // The forward direction: a build that predates placements ignores tags it does not know, so a
    // face sketch opens there as a plain global sketch rather than failing. That only works if the
    // `plane` line is still present and still correct, which is why it is written even when a face
    // is set. Asserted on the TEXT because the property is about the format, not the object.
    SketchPlane placement;
    placement.kind = SketchPlane::Kind::Face;
    placement.global = Plane::XZ;
    placement.face = "Box#1/face:Z+";

    const std::string text = Sketch(placement).serialize();

    CHECK(text.find("plane XZ\n") != std::string::npos);
    CHECK(text.find("plane_kind 1\n") != std::string::npos);
    CHECK(text.find("plane_face Box#1/face:Z+\n") != std::string::npos);
}

TEST_CASE("an unknown placement kind falls back to global rather than failing",
          "[sketch][plane]") {
    // A file from a FUTURE build carrying a datum kind this one does not implement. Refusing it
    // would make the format unextendable; guessing at the geometry would put the sketch somewhere
    // the user never drew it. Reading it as global keeps the geometry and the plane it recorded.
    const std::string future =
        "sketch 1\n"
        "plane XY\n"
        "plane_kind 7\n"
        "g 1 line 0 0 0 10 0\n";

    const auto parsed = Sketch::deserialize(future);
    REQUIRE(parsed);
    CHECK(parsed.value().placement().kind == SketchPlane::Kind::Global);
    CHECK(parsed.value().geometry().size() == 1);
}

// ── the loose end: an unplaced sketch must refuse, not guess ────────────────────────────

TEST_CASE("a face sketch with no resolved frame refuses to build a profile", "[sketch][plane]") {
    // The whole point of the placement work. Before this check, a face-placed sketch fell through
    // to the global plane and built its profile there — silently, with every downstream feature
    // agreeing, and the geometry sitting somewhere the user never drew it.
    SketchPlane placement;
    placement.kind = SketchPlane::Kind::Face;
    placement.face = "Box#1/face:Z+";

    Sketch sketch(placement);
    sketch.addLine(0, 0, 10, 0);
    sketch.addLine(10, 0, 10, 10);
    sketch.addLine(10, 10, 0, 0);

    CHECK_FALSE(sketch.isPlaced());

    const auto wire = sketch.toWire();
    REQUIRE_FALSE(wire);
    CHECK_FALSE(wire.error().message.empty());
}

TEST_CASE("a resolved frame places geometry where the frame says", "[sketch][plane]") {
    SketchPlane placement;
    placement.kind = SketchPlane::Kind::Face;
    placement.face = "Box#1/face:Z+";

    Sketch sketch(placement);

    // A face 40mm up, with u along +X and v along +Y: the top of a box, which is the case a user
    // hits first.
    SketchFrame frame;
    frame.origin[2] = 40.0;
    sketch.setResolvedFrame(frame);

    CHECK(sketch.isPlaced());

    const auto p = sketch.to3d(3.0, 4.0);
    CHECK(p[0] == 3.0);
    CHECK(p[1] == 4.0);
    CHECK(p[2] == 40.0);   // ON the face, not on XY

    // And the normal follows the frame rather than a global axis.
    const auto n = frame.normal();
    CHECK(n[2] == 1.0);
}

TEST_CASE("a global sketch is placed without any resolution", "[sketch][plane]") {
    // The property that keeps every existing document working: a global-plane sketch has always
    // known where it is, so it must never require a frame.
    Sketch sketch(Plane::XZ);
    CHECK(sketch.isPlaced());
    CHECK_FALSE(sketch.needsResolution());

    const auto p = sketch.to3d(3.0, 4.0);
    CHECK(p[0] == 3.0);
    CHECK(p[1] == 0.0);
    CHECK(p[2] == 4.0);
}

// ── measuring a face, which is what 1b resolves a reference INTO ────────────────────────

TEST_CASE("a planar face measures into a usable frame", "[sketch][plane][kernel]") {
    auto box = cad::kernel::makeBox(40, 30, 20);
    REQUIRE(box);

    // Any planar face of a box. The frame's normal must be a unit axis and its axes orthogonal —
    // if either fails, a sketch placed here would be skewed rather than merely misplaced.
    const auto faces = box.value().op.shape().subShapes(cad::kernel::ShapeType::Face);
    REQUIRE_FALSE(faces.empty());

    const auto frame = cad::kernel::planeOf(faces.front());
    REQUIRE(frame);

    const auto& f = frame.value();
    const double nlen = std::sqrt(f.normal[0] * f.normal[0] + f.normal[1] * f.normal[1]
                                  + f.normal[2] * f.normal[2]);
    CHECK(nlen == Catch::Approx(1.0));

    const double dot = f.u[0] * f.v[0] + f.u[1] * f.v[1] + f.u[2] * f.v[2];
    CHECK(dot == Catch::Approx(0.0).margin(1e-12));
}

TEST_CASE("measuring the same face twice agrees", "[sketch][plane][kernel]") {
    // The property that stops a sketch rotating on its own face between rebuilds. OCCT's own
    // parameterisation is used precisely so this holds rather than depending on a choice made here.
    auto box = cad::kernel::makeBox(40, 30, 20);
    REQUIRE(box);
    const auto face = box.value().op.shape().subShapes(cad::kernel::ShapeType::Face).front();

    const auto a = cad::kernel::planeOf(face);
    const auto b = cad::kernel::planeOf(face);
    REQUIRE(a);
    REQUIRE(b);
    for (int i = 0; i < 3; ++i) {
        CHECK(a.value().origin[i] == b.value().origin[i]);
        CHECK(a.value().u[i] == b.value().u[i]);
        CHECK(a.value().normal[i] == b.value().normal[i]);
    }
}

TEST_CASE("a non-planar face is refused rather than approximated", "[sketch][plane][kernel]") {
    // A cylinder's side has no single plane. Choosing one would place a sketch somewhere the user
    // never picked — the same silent misplacement isPlaced() exists to stop.
    auto cyl = cad::kernel::makeCylinder(10, 30);
    REQUIRE(cyl);

    bool sawCurvedRefusal = false;
    for (const auto& face : cyl.value().shape().subShapes(cad::kernel::ShapeType::Face)) {
        if (!cad::kernel::planeOf(face)) sawCurvedRefusal = true;
    }
    CHECK(sawCurvedRefusal);
}

// ── end to end: a sketch placed on a real face, resolved by the recompute ───────────────

TEST_CASE("a sketch on a box face is located on that face", "[sketch][plane][recompute]") {
    using namespace cad::document;

    Document doc;
    auto [withBox, boxId] = doc.add("Box");
    doc = withBox;
    doc = doc.replace(std::make_shared<const ObjectData>(
        doc.find(boxId)->withProperty("dx", cad::units::millimetres(40))
            .withProperty("dy", cad::units::millimetres(30))
            .withProperty("dz", cad::units::millimetres(20))));

    const auto registry = cad::features::builtins();
    cad::recompute::MemoryCache cache;
    cad::recompute::Engine engine(registry, cache);

    auto first = engine.recompute(doc);
    REQUIRE(first);
    doc = first.value().first;

    // A real name from the box's own element map — never an index, which is the entire point.
    const auto* boxOut = doc.find(boxId)->output();
    REQUIRE(boxOut != nullptr);
    const auto names = boxOut->map.allNames();
    REQUIRE_FALSE(names.empty());

    // The first face whose plane can actually be measured.
    std::string faceText;
    cad::kernel::PlaneFrame expected;
    for (const auto& n : names) {
        const auto resolved = boxOut->map.resolve(n);
        if (!resolved) continue;
        const auto measured = cad::kernel::planeOf(*resolved);
        if (!measured) continue;
        faceText = n.toString();
        expected = measured.value();
        break;
    }
    REQUIRE_FALSE(faceText.empty());

    SketchPlane placement;
    placement.kind = SketchPlane::Kind::Face;
    placement.face = faceText;
    Sketch profile(placement);
    profile.addLine(0, 0, 5, 0);
    profile.addLine(5, 0, 5, 5);
    profile.addLine(5, 5, 0, 0);

    auto [withSketch, sketchId] = doc.add("Sketch");
    doc = withSketch;
    doc = doc.replace(std::make_shared<const ObjectData>(
        doc.find(sketchId)->withProperty("sketch", profile.serialize())
            // THE INPUT. Not decoration: it puts the box in ctx.inputs so the face can be
            // resolved, AND folds the box's cache key into the sketch's, so moving the face
            // invalidates the sketch instead of leaving it cached at the old position.
            .withProperty("body", boxId)));

    auto second = engine.recompute(doc);
    REQUIRE(second);
    doc = second.value().first;

    const auto sketchObject = doc.find(sketchId);
    REQUIRE(sketchObject);
    INFO("sketch state: " << static_cast<int>(sketchObject->state())
                          << " error: " << sketchObject->error().message);
    CHECK(sketchObject->state() == ObjectState::Clean);
    REQUIRE(sketchObject->output() != nullptr);

    // The profile must sit ON the measured face, not on XY. Checked through the wire's own
    // location rather than by trusting the frame we handed in.
    // Project the profile's centroid onto the face normal. Every point of a sketch drawn on a
    // plane has the same projection as the plane's origin — so this is the check that the geometry
    // is ON the face rather than merely near it, and it does not depend on which face was picked.
    const auto centroid = sketchObject->output()->shape.measure();
    const double n[3]{expected.normal[0], expected.normal[1], expected.normal[2]};
    const double along = n[0] * centroid.cx + n[1] * centroid.cy + n[2] * centroid.cz;
    const double origin = n[0] * expected.origin[0] + n[1] * expected.origin[1]
                          + n[2] * expected.origin[2];
    CHECK(along == Catch::Approx(origin).margin(1e-6));
}

TEST_CASE("a sketch naming a face that does not exist is blocked, not moved",
          "[sketch][plane][recompute]") {
    using namespace cad::document;

    Document doc;
    auto [withBox, boxId] = doc.add("Box");
    doc = withBox;
    doc = doc.replace(std::make_shared<const ObjectData>(
        doc.find(boxId)->withProperty("dx", cad::units::millimetres(10))
            .withProperty("dy", cad::units::millimetres(10))
            .withProperty("dz", cad::units::millimetres(10))));

    SketchPlane placement;
    placement.kind = SketchPlane::Kind::Face;
    placement.face = "NoSuchFeature#99/face:Nowhere";
    Sketch profile(placement);
    profile.addLine(0, 0, 5, 0);

    auto [withSketch, sketchId] = doc.add("Sketch");
    doc = withSketch;
    doc = doc.replace(std::make_shared<const ObjectData>(
        doc.find(sketchId)->withProperty("sketch", profile.serialize())
            .withProperty("body", boxId)));

    const auto registry = cad::features::builtins();
    cad::recompute::MemoryCache cache;
    cad::recompute::Engine engine(registry, cache);
    auto result = engine.recompute(doc);
    REQUIRE(result);

    const auto sketchObject = result.value().first.find(sketchId);
    REQUIRE(sketchObject);

    // Failed with a reason — NEVER silently relocated to XY. A fallback would move a user's
    // geometry and nothing downstream would know it had happened.
    CHECK(sketchObject->state() != ObjectState::Clean);
    CHECK_FALSE(sketchObject->error().message.empty());
}
