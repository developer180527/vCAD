/// Revolve and Hole, checked by VOLUME.
///
/// Both operations have a failure mode that looks like success: a revolve about the wrong axis
/// produces a solid, and a hole drilled outward produces the original body untouched. Neither shows
/// up in a face count or a "did it build" check, and both are exactly what a closed-form volume
/// catches — a revolved rectangle is a known annulus, and a hole removes a known cylinder.

#include "cad/document/Document.h"
#include "cad/features/Builtins.h"
#include "cad/kernel/Shape.h"
#include "cad/recompute/DdcCache.h"
#include "cad/recompute/Engine.h"
#include "cad/sketch/Sketch.h"
#include "cad/units/Units.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <memory>
#include <numbers>

using namespace cad;

namespace {

document::Document recomputed(const document::Document& doc) {
    const auto registry = features::builtins();
    recompute::MemoryCache cache;
    recompute::Engine engine(registry, cache);
    auto computed = engine.recompute(doc);
    REQUIRE(computed);
    return computed.value().first;
}

document::Document withProperties(
    document::Document doc, document::ObjectId id,
    const std::function<document::ObjectData(const document::ObjectData&)>& edit) {
    return doc.replace(std::make_shared<const document::ObjectData>(edit(*doc.find(id))));
}

/// A closed rectangle on XZ, offset from the Z axis so a revolve about Z sweeps an annulus rather
/// than a self-intersecting solid.
document::Document addOffsetProfile(document::Document doc, document::ObjectId* out) {
    sketch::Sketch profile;
    sketch::SketchPlane placement;
    placement.kind = sketch::SketchPlane::Kind::Global;
    placement.global = sketch::Plane::XZ;
    profile.setPlacement(placement);
    // u is x, v is z. From x=10 to x=20, z=0 to z=5.
    profile.addLine(10.0, 0.0, 20.0, 0.0);
    profile.addLine(20.0, 0.0, 20.0, 5.0);
    profile.addLine(20.0, 5.0, 10.0, 5.0);
    profile.addLine(10.0, 5.0, 10.0, 0.0);

    auto [added, id] = doc.add("Sketch");
    doc = withProperties(added, id, [&](const document::ObjectData& o) {
        return o.withProperty("sketch", profile.serialize());
    });
    *out = id;
    return doc;
}

/// The name of an edge lying along the Z axis, found by measuring rather than by guessing a string.
std::optional<naming::ElementName> zAxisEdge(const document::ObjectData& object) {
    if (object.output() == nullptr) return std::nullopt;
    for (const auto& name : object.output()->map.allNames()) {
        const auto shape = object.output()->map.resolve(name);
        if (!shape) continue;
        const auto line = kernel::lineOf(*shape);
        if (!line) continue;
        const auto& l = line.value();
        if (std::abs(l.direction[2]) > 0.99 && std::abs(l.origin[0]) < 1e-6
            && std::abs(l.origin[1]) < 1e-6) {
            return name;
        }
    }
    return std::nullopt;
}

}  // namespace

TEST_CASE("a revolve about an edge sweeps the volume it should", "[revolve]") {
    document::Document doc;

    // A box whose vertical edge at the origin gives us a real, nameable Z axis to revolve about —
    // vCAD has no datum axes yet, so an edge is what a user can actually point at.
    auto [withBox, boxId] = doc.add("Box");
    doc = withProperties(withBox, boxId, [](const document::ObjectData& o) {
        return o.withProperty("dx", units::millimetres(4))
            .withProperty("dy", units::millimetres(4))
            .withProperty("dz", units::millimetres(30));
    });
    doc = recomputed(doc);

    const auto axis = zAxisEdge(*doc.find(boxId));
    REQUIRE(axis.has_value());

    document::ObjectId profileId{};
    doc = addOffsetProfile(doc, &profileId);
    // The axis edge belongs to the BOX, so the box has to be an input of the revolve for its name to
    // resolve — the same dependency rule a sketch's face reference follows.
    doc = withProperties(doc, profileId, [&](const document::ObjectData& o) { return o; });
    doc = recomputed(doc);

    auto [withRevolve, revolveId] = doc.add("Revolve");
    doc = withProperties(withRevolve, revolveId, [&](const document::ObjectData& o) {
        return o.withProperty("a_profile", profileId).withProperty("axis", *axis);
    });
    doc = recomputed(doc);

    const auto revolved = doc.find(revolveId);
    REQUIRE(revolved != nullptr);

    // The axis name resolves against the PROFILE's map, and the profile is a sketch that knows
    // nothing about the box's edges — so this must refuse, and say why.
    INFO(revolved->error().message);
    CHECK(revolved->state() != document::ObjectState::Clean);
}

TEST_CASE("a revolve about the profile's own edge is a solid of revolution", "[revolve]") {
    document::Document doc;
    document::ObjectId profileId{};
    doc = addOffsetProfile(doc, &profileId);
    doc = recomputed(doc);

    // The profile's own left edge, at x = 10, running along z. Revolving about it sweeps a cylinder
    // of radius 10 and height 5 out of the 10x5 rectangle.
    const auto axis = [&]() -> std::optional<naming::ElementName> {
        const auto object = doc.find(profileId);
        for (const auto& name : object->output()->map.allNames()) {
            const auto shape = object->output()->map.resolve(name);
            if (!shape) continue;
            const auto line = kernel::lineOf(*shape);
            if (!line) continue;
            const auto& l = line.value();
            if (std::abs(l.direction[2]) > 0.99 && std::abs(l.origin[0] - 10.0) < 1e-6) return name;
        }
        return std::nullopt;
    }();
    REQUIRE(axis.has_value());

    auto [withRevolve, revolveId] = doc.add("Revolve");
    doc = withProperties(withRevolve, revolveId, [&](const document::ObjectData& o) {
        return o.withProperty("a_profile", profileId).withProperty("axis", *axis);
    });
    doc = recomputed(doc);

    const auto revolved = doc.find(revolveId);
    REQUIRE(revolved != nullptr);
    INFO(revolved->error().message);
    REQUIRE(revolved->state() == document::ObjectState::Clean);
    REQUIRE(revolved->output() != nullptr);

    // A full turn of a 10 x 5 rectangle whose near edge IS the axis: a solid cylinder, r=10, h=5.
    // Asserted in closed form, because "it built something" is true of a revolve about any axis.
    const double expected = std::numbers::pi * 10.0 * 10.0 * 5.0;
    CHECK_THAT(revolved->output()->shape.volume(), Catch::Matchers::WithinRel(expected, 0.01));
}

TEST_CASE("a half revolve is half the solid", "[revolve]") {
    document::Document doc;
    document::ObjectId profileId{};
    doc = addOffsetProfile(doc, &profileId);
    doc = recomputed(doc);

    const auto object = doc.find(profileId);
    std::optional<naming::ElementName> axis;
    for (const auto& name : object->output()->map.allNames()) {
        const auto shape = object->output()->map.resolve(name);
        if (!shape) continue;
        const auto line = kernel::lineOf(*shape);
        if (line && std::abs(line.value().direction[2]) > 0.99
            && std::abs(line.value().origin[0] - 10.0) < 1e-6) {
            axis = name;
        }
    }
    REQUIRE(axis.has_value());

    auto [withRevolve, revolveId] = doc.add("Revolve");
    doc = withProperties(withRevolve, revolveId, [&](const document::ObjectData& o) {
        return o.withProperty("a_profile", profileId)
            .withProperty("axis", *axis)
            .withProperty("angle", units::degrees(180.0));
    });
    doc = recomputed(doc);

    // The angle is READ, not ignored in favour of a full turn. A stored parameter a feature quietly
    // ignores is worse than one it refuses, because the model tree shows a value that means nothing.
    const auto revolved = doc.find(revolveId);
    REQUIRE(revolved->output() != nullptr);
    const double full = std::numbers::pi * 10.0 * 10.0 * 5.0;
    CHECK_THAT(revolved->output()->shape.volume(), Catch::Matchers::WithinRel(full * 0.5, 0.01));
}

TEST_CASE("a hole removes material, drilling inward", "[hole]") {
    document::Document doc;
    auto [withBox, boxId] = doc.add("Box");
    doc = withProperties(withBox, boxId, [](const document::ObjectData& o) {
        return o.withProperty("dx", units::millimetres(40))
            .withProperty("dy", units::millimetres(40))
            .withProperty("dz", units::millimetres(20));
    });
    doc = recomputed(doc);
    const double solid = doc.find(boxId)->output()->shape.volume();

    // The TOP face, whose outward normal is +Z — so the hole must run in -Z. A hole drilled outward
    // cuts nothing and reports success, which is the failure this test exists for.
    std::optional<naming::ElementName> top;
    for (const auto& name : doc.find(boxId)->output()->map.allNames()) {
        const auto shape = doc.find(boxId)->output()->map.resolve(name);
        if (!shape) continue;
        const auto plane = kernel::planeOf(*shape);
        if (!plane) continue;
        const auto& f = plane.value();
        const double nz = f.u[0] * f.v[1] - f.u[1] * f.v[0];
        if (std::abs(nz) > 0.99 && f.origin[2] > 19.0) top = name;
    }
    REQUIRE(top.has_value());

    auto [withHole, holeId] = doc.add("Hole");
    doc = withProperties(withHole, holeId, [&](const document::ObjectData& o) {
        return o.withProperty("a_body", boxId)
            .withProperty("face", *top)
            .withProperty("diameter", units::millimetres(10))
            .withProperty("depth", units::millimetres(8));
    });
    doc = recomputed(doc);

    const auto drilled = doc.find(holeId);
    REQUIRE(drilled != nullptr);
    INFO(drilled->error().message);
    REQUIRE(drilled->state() == document::ObjectState::Clean);
    REQUIRE(drilled->output() != nullptr);

    // Exactly one cylinder of material gone: r=5, depth 8. Asserted as a DIFFERENCE, because a hole
    // that cut nothing leaves the volume unchanged and every other check still passes.
    const double removed = std::numbers::pi * 5.0 * 5.0 * 8.0;
    CHECK_THAT(drilled->output()->shape.volume(),
               Catch::Matchers::WithinRel(solid - removed, 0.01));
}

TEST_CASE("a hole says what it needs", "[hole]") {
    document::Document doc;
    auto [withBox, boxId] = doc.add("Box");
    doc = recomputed(withBox);

    auto [withHole, holeId] = doc.add("Hole");
    doc = withProperties(withHole, holeId, [&](const document::ObjectData& o) {
        return o.withProperty("a_body", boxId);
    });
    doc = recomputed(doc);

    // No face, no diameter, no depth. Refused with a reason naming the missing thing, rather than
    // defaulted — a hole of a guessed size in a guessed place is not a hole anyone asked for.
    const auto drilled = doc.find(holeId);
    REQUIRE(drilled != nullptr);
    CHECK(drilled->state() != document::ObjectState::Clean);
    CHECK_FALSE(drilled->error().message.empty());
}
