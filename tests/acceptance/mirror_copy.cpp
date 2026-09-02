// A mirrored copy, and what happens when it meets the body it was copied from.
//
// `nameCopy` has only ever been exercised against `translate`, with the copy placed 200 mm from its
// source. Two things about that fixture were doing quiet work, and both are wrong for a Mirror
// feature:
//
// It was a TRANSLATION. A reflection is the transform most likely to break anything that leans on
// geometry rather than provenance — a mirrored face has the same area, the same shape and a
// different handedness, so a tie-breaker comparing measurements sees a twin where a translation
// gave it a stranger.
//
// And the copy was FAR AWAY. Bodies 200 mm apart fuse without a single face splitting, so every
// existing copy test takes the easy path through `propagate`. The whole point of a mirror is that
// the copy lands against the original: they touch, or they overlap, and faces split. That is the
// split-face discrimination work and the copy work meeting for the first time, and neither was
// written with the other in mind.
//
// The box spans x 0..40, so the mirror plane is what decides which case is being tested: x = -100
// puts the copy clear of the source, x = 0 puts them face to face, x = 30 makes them overlap.

#include "Model.h"

#include "cad/kernel/Booleans.h"
#include "cad/kernel/Transform.h"
#include "cad/naming/ElementMap.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

using cadtest::box;
using cadtest::faceName;
using cad::kernel::BoxFace;

namespace {

constexpr double kX[3] = {1.0, 0.0, 0.0};

/// The source body, its names, and a mirrored copy of it reflected in the plane x = `planeX`.
///
/// Returned together because every test here needs both halves: the copy is only interesting in
/// relation to the source it must stay distinct from.
struct Mirrored {
    cadtest::Model source;
    cad::kernel::Shape copyShape;
    cad::naming::ElementMap copyMap;
};

Mirrored mirroredAt(double planeX, cad::naming::NamingContext::Instance instance = {1, 0}) {
    auto source = box(40.0, 30.0, 20.0, 1);
    REQUIRE(source.ok());

    const double origin[3] = {planeX, 0.0, 0.0};
    auto reflected = cad::kernel::mirror(source.value().shape, origin, kX);
    REQUIRE(reflected.ok());

    cad::naming::NamingContext ctx(2, 0);
    auto map = ctx.nameCopy(reflected.value(), source.value().shape, source.value().map, instance);
    if (!map.ok()) FAIL(map.error().message << " | " << map.error().detail);

    return Mirrored{std::move(source.value()), reflected.value().shape(), std::move(map.value())};
}

}  // namespace

TEST_CASE("a mirrored copy is a different thing from its source", "[mirror][naming][multiplicity]") {
    // The same property the translated copy has, asserted for a reflection — because the reason it
    // holds is provenance, not distance, and a test that only ever moved things 200 mm away could
    // not tell those two explanations apart.
    const auto m = mirroredAt(-100.0);
    const auto top = faceName(m.source, BoxFace::ZMax);
    REQUIRE(top.ok());

    CHECK(m.copyMap.size() == m.source.map.size());          // nothing lost in the stamping
    CHECK_FALSE(m.copyMap.resolve(top.value()).has_value()); // not the source's names
    CHECK(m.source.map.resolve(top.value()).has_value());    // and the source is untouched
}

TEST_CASE("a mirrored copy is stable across rebuilds", "[mirror][naming][multiplicity]") {
    // A reflection is the case where a geometric tie-breaker would be most tempted to wobble: the
    // copy's faces have exactly the source's areas. If anything in the path sorted on measurement
    // alone, this is where it would come out differently on a second run.
    const auto namesOf = [](cad::naming::NamingContext::Instance instance) {
        const auto m = mirroredAt(-100.0, instance);
        std::vector<std::string> names;
        for (const auto& name : m.copyMap.allNames()) names.push_back(name.toString());
        std::sort(names.begin(), names.end());
        return names;
    };

    CHECK(namesOf({1, 0}) == namesOf({1, 0}));
    CHECK(namesOf({1, 0}) != namesOf({2, 0}));
}

TEST_CASE("a mirrored copy fuses back onto its source", "[mirror][naming][multiplicity]") {
    // The disjoint case, which is the one the existing copy tests cover — included here as the
    // baseline the two below depart from, so a failure in them can be read as being about contact
    // rather than about mirroring.
    const auto m = mirroredAt(-100.0);

    auto fused = cad::kernel::booleanFuse(m.source.shape, m.copyShape);
    REQUIRE(fused.ok());

    cad::naming::NamingContext ctx(3, 0);
    const cad::kernel::Shape* a = &m.source.shape;
    const cad::kernel::Shape* b = &m.copyShape;
    auto map = ctx.propagate(fused.value(), {a, b}, {&m.source.map, &m.copyMap});
    if (!map.ok()) FAIL(map.error().message << " | " << map.error().detail);

    CHECK(map.value().unnamed(fused.value().shape()).empty());
    CHECK(map.value().collisions().empty());
}

TEST_CASE("a mirrored copy touching its source face to face", "[mirror][naming][splits]") {
    // Mirroring in x = 0 puts the copy at x -40..0, sharing a whole face with the source. This is
    // what a Mirror feature does by default — reflect about the plane the part was designed
    // against — and it is the case where the two faces that meet are ANNIHILATED by the fuse
    // rather than split. Both had names. Neither survives, and that must be a clean outcome
    // rather than a naming loss.
    const auto m = mirroredAt(0.0);

    auto fused = cad::kernel::booleanFuse(m.source.shape, m.copyShape);
    REQUIRE(fused.ok());
    const auto result = fused.value().shape();

    // One body twice the width, not two bodies touching.
    CHECK(result.volume() > m.source.shape.volume() * 1.9);

    cad::naming::NamingContext ctx(3, 0);
    const cad::kernel::Shape* a = &m.source.shape;
    const cad::kernel::Shape* b = &m.copyShape;
    auto map = ctx.propagate(fused.value(), {a, b}, {&m.source.map, &m.copyMap});
    if (!map.ok()) FAIL(map.error().message << " | " << map.error().detail);

    CHECK(map.value().unnamed(result).empty());
    CHECK(map.value().collisions().empty());
}

TEST_CASE("a mirrored copy overlapping its source", "[mirror][naming][splits]") {
    // The hard one, and the reason this file exists. Mirroring in x = 30 puts the copy at
    // x 20..60, overlapping the source's 20..40. Faces SPLIT — and they split into pieces whose
    // twins across the mirror plane have identical areas, which is precisely the tie the
    // discriminator refuses to guess at.
    //
    // Written first to accept either outcome — clean names, or an honest NamingLost refusal —
    // because it was not obvious which the discriminator would reach. It succeeds: every element
    // comes out named and distinct. So the hedge is gone, and this asserts the real answer.
    //
    // Which matters, because a test that accepts both cannot notice the day it stops being the
    // first one. Under a mutation that stops `nameCopy` stamping, the hedged version passed by
    // taking the refusal branch; this one fails, which is the whole point of writing it down.
    const auto m = mirroredAt(30.0);

    auto fused = cad::kernel::booleanFuse(m.source.shape, m.copyShape);
    REQUIRE(fused.ok());
    const auto result = fused.value().shape();
    CHECK(result.volume() > m.source.shape.volume());   // they really did overlap and merge

    cad::naming::NamingContext ctx(3, 0);
    const cad::kernel::Shape* a = &m.source.shape;
    const cad::kernel::Shape* b = &m.copyShape;
    auto map = ctx.propagate(fused.value(), {a, b}, {&m.source.map, &m.copyMap});

    if (!map.ok()) FAIL(map.error().message << " | " << map.error().detail);
    CHECK(map.value().unnamed(result).empty());
    CHECK(map.value().collisions().empty());
}

TEST_CASE("a mirror of a mirror is distinct from the original", "[mirror][naming][multiplicity]") {
    // Two reflections compose to a translation, so the twice-mirrored body sits somewhere the
    // source is not and has the source's handedness back. It is still a COPY, and its names must
    // still say so — a scheme that recovered the source's names here would be one where mirroring
    // twice quietly re-identified a body as its own original.
    auto source = box(40.0, 30.0, 20.0, 1);
    REQUIRE(source.ok());
    const auto top = faceName(source.value(), BoxFace::ZMax);
    REQUIRE(top.ok());

    const double first[3] = {-100.0, 0.0, 0.0};
    auto once = cad::kernel::mirror(source.value().shape, first, kX);
    REQUIRE(once.ok());
    cad::naming::NamingContext ctxA(2, 0);
    auto mapA = ctxA.nameCopy(once.value(), source.value().shape, source.value().map, {1, 0});
    if (!mapA.ok()) FAIL(mapA.error().message << " | " << mapA.error().detail);
    const auto shapeA = once.value().shape();

    const double second[3] = {-300.0, 0.0, 0.0};
    auto twice = cad::kernel::mirror(shapeA, second, kX);
    REQUIRE(twice.ok());
    cad::naming::NamingContext ctxB(3, 0);
    auto mapB = ctxB.nameCopy(twice.value(), shapeA, mapA.value(), {2, 0});
    if (!mapB.ok()) FAIL(mapB.error().message << " | " << mapB.error().detail);

    // Not the source's names, and not the first copy's either.
    CHECK_FALSE(mapB.value().resolve(top.value()).has_value());
    for (const auto& name : mapA.value().allNames()) {
        INFO(name.toString());
        CHECK_FALSE(mapB.value().resolve(name).has_value());
    }
}
