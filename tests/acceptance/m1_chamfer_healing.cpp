#include "Model.h"
#include "cad/kernel/Healing.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

using namespace cadtest;
using cad::kernel::BoxFace;
using cad::naming::NameStep;
using cad::naming::Provenance;

TEST_CASE("M1: a chamfered edge survives upstream parameter changes", "[m1][naming]") {
    // Same contract as the fillet sweep. Chamfer is a separate OCCT algorithm with a
    // different Add() signature, so it needs its own proof that naming holds.
    auto nominal = box(100.0, 60.0, 40.0);
    REQUIRE(nominal.ok());
    auto pickedR = edgeBetween(nominal.value(), BoxFace::ZMax, BoxFace::YMin);
    REQUIRE(pickedR.ok());
    const ElementName picked = pickedR.value();

    const double width = GENERATE(range(30.0, 200.0, 10.0));
    INFO("width = " << width);

    auto rebuilt = box(width, 60.0, 40.0);
    REQUIRE(rebuilt.ok());

    auto chamfered = chamfer(rebuilt.value(), picked, 4.0);
    REQUIRE(chamfered.ok());
    CHECK(chamfered.value().shape.validate().ok());

    // The chamfer face derives from the edge the user picked.
    NameStep s;
    s.featureSerial = 2;
    s.opTag = 0;
    s.provenance = Provenance::Generated;
    s.parents = {picked.digest()};
    CHECK(chamfered.value().map.resolve(picked.derive(s)).has_value());

    CHECK(chamfered.value().map.unnamed(chamfered.value().shape).empty());
}

TEST_CASE("M1: chamfer refuses a lost reference", "[m1][naming]") {
    auto base = box(100.0, 60.0, 40.0);
    REQUIRE(base.ok());
    auto picked = edgeBetween(base.value(), BoxFace::ZMax, BoxFace::YMin);
    REQUIRE(picked.ok());

    auto toolBox = box(140.0, 30.0, 30.0, /*serial*/ 10);
    REQUIRE(toolBox.ok());
    auto tool = translated(toolBox.value(), -20.0, -10.0, 30.0);
    REQUIRE(tool.ok());
    auto chopped = cut(base.value(), tool.value());
    REQUIRE(chopped.ok());

    auto result = chamfer(chopped.value(), picked.value(), 3.0);
    REQUIRE_FALSE(result.ok());
    CHECK(result.error().code == ErrorCode::NamingLost);
}

// ---------------------------------------------------------------------------------------

TEST_CASE("M1: healing leaves sound geometry alone", "[m1][healing]") {
    auto m = box(100.0, 60.0, 40.0);
    REQUIRE(m.ok());

    auto before = cad::kernel::inspect(m.value().shape);
    REQUIRE(before.ok());
    REQUIRE(before.value().wasValid);

    Shape s = m.value().shape;
    auto report = cad::kernel::heal(s);
    REQUIRE(report.ok());
    CHECK(report.value().wasValid);
    CHECK(report.value().isValidNow);
    // Not touching a sound shape matters: healing changes topology, which would invalidate
    // every element name attached to it.
    CHECK_FALSE(report.value().changed);
    CHECK(report.value().actions.empty());
    CHECK(report.value().summary().find("no repairs needed") != std::string::npos);
}

TEST_CASE("M1: healing reports what it found on broken geometry", "[m1][healing]") {
    // An open shell: a box with one face removed, declared as a solid. BRepCheck rejects it,
    // which is the class of damage that arrives routinely in foreign STEP and IGES.
    auto broken = openShellSolid(100.0, 60.0, 40.0);
    REQUIRE(broken.ok());

    auto before = cad::kernel::inspect(broken.value());
    REQUIRE(before.ok());
    REQUIRE_FALSE(before.value().wasValid);

    Shape s = broken.value();
    auto report = cad::kernel::heal(s);
    REQUIRE(report.ok());

    CHECK_FALSE(report.value().wasValid);
    CHECK(report.value().changed);

    // Every face and edge of an unclosed shell is individually sound — the defect is in how
    // they are assembled. The report must say that rather than print "0 faces and 0 edges",
    // which is technically true and completely useless to the person reading it.
    CHECK(report.value().structuralDefect);
    CHECK(report.value().invalidFaces == 0);

    // The point is not that every defect is repairable — it is that the outcome is
    // reported rather than silently assumed. docs/FORMATS.md rule 1.
    CHECK_FALSE(report.value().actions.empty());
    INFO(report.value().summary());
    CHECK(report.value().summary().find("closed solid") != std::string::npos);
    CHECK(report.value().summary().find("0 face") == std::string::npos);
}

TEST_CASE("M1: healing is idempotent", "[m1][healing]") {
    auto broken = openShellSolid(100.0, 60.0, 40.0);
    REQUIRE(broken.ok());

    Shape s = broken.value();
    auto first = cad::kernel::heal(s);
    REQUIRE(first.ok());

    auto second = cad::kernel::heal(s);
    REQUIRE(second.ok());
    // Whatever state the first pass reached, a second pass must not keep churning it.
    CHECK(second.value().isValidNow == first.value().isValidNow);
    if (first.value().isValidNow) {
        CHECK_FALSE(second.value().changed);
    }
}

TEST_CASE("M1: unifySameDomain is opt-in", "[m1][healing]") {
    // It merges coplanar faces, which changes topology and therefore element names. That is
    // a modelling decision, not a repair, so it must never happen implicitly during import.
    auto a = box(100.0, 60.0, 40.0);
    auto bBase = box(100.0, 60.0, 40.0, /*serial*/ 20);
    REQUIRE(a.ok());
    REQUIRE(bBase.ok());
    auto b = translated(bBase.value(), 100.0, 0.0, 0.0);
    REQUIRE(b.ok());
    auto fused = fuseOnly(a.value(), b.value());
    REQUIRE(fused.ok());

    const auto facesBefore = fused.value().shape.subShapes(cad::kernel::ShapeType::Face).size();

    Shape untouched = fused.value().shape;
    auto plain = cad::kernel::heal(untouched);
    REQUIRE(plain.ok());
    CHECK(untouched.subShapes(cad::kernel::ShapeType::Face).size() == facesBefore);

    Shape merged = fused.value().shape;
    cad::kernel::HealingOptions opts;
    opts.unifySameDomain = true;
    auto unified = cad::kernel::heal(merged, opts);
    REQUIRE(unified.ok());
    CHECK(merged.subShapes(cad::kernel::ShapeType::Face).size() < facesBefore);
}
