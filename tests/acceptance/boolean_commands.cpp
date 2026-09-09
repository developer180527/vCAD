// Join, Intersect and Chamfer, driven through the command catalogue.
//
// The geometry underneath these was never in doubt -- `booleanFuse`, `booleanCommon` and
// `chamferEdges` are exercised all over the suite by direct calls. What had no test at all was the
// path a user actually takes: find the command in the catalogue, ask whether it is enabled with a
// given selection, invoke it.
//
// That is not a hypothetical gap. Hole and Revolve both computed correctly for days while being
// UNREACHABLE, because the enable predicate and the feature's requirements had drifted apart and
// nothing tested the catalogue. Measure made it three. Every one of those was invisible to a suite
// that called the compute directly.
//
// `booleanCommon` is the sharper case: before this file it was the one kernel operation with no
// test on any path, direct or otherwise.

#include "cad/app/Controller.h"
#include "cad/kernel/Shape.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <utility>

using cad::app::Controller;
using cad::document::ObjectId;
using Catch::Approx;

namespace {

const cad::app::Command* commandNamed(const Controller& app, const std::string& id) {
    for (const auto& command : app.commands()) {
        if (command.id == id) return &command;
    }
    return nullptr;
}

double volumeOf(const Controller& app, ObjectId id) {
    const auto object = app.document().find(id);
    if (!object || object->output() == nullptr) return -1.0;
    return object->output()->shape.volume();
}

/// The error a feature is showing in the browser, or "" if it computed.
std::string errorOf(const Controller& app, ObjectId id) {
    for (const auto& item : app.tree()) {
        if (item.id == id) return item.error;
    }
    return "<no such row>";
}

/// Runs a catalogue command, asserting it was reachable rather than assuming it.
ObjectId invokeCommand(Controller& app, const std::string& id) {
    const auto* command = commandNamed(app, id);
    REQUIRE(command != nullptr);
    // The assertion that matters: a command that exists but is disabled with the right selection
    // is exactly the failure this file was written for.
    REQUIRE(command->enabled(app.context()));
    command->invoke();
    app.refresh();
    REQUIRE(app.selection().size() == 1);
    return app.selection().front();
}

ObjectId aBox(Controller& app) {
    return invokeCommand(app, "feature.box");
}

/// The default box, 100 x 60 x 40, spanning x 0..100.
constexpr double kDx = 100.0;
constexpr double kDy = 60.0;
constexpr double kDz = 40.0;
constexpr double kBoxVolume = kDx * kDy * kDz;

/// The second box, deliberately a DIFFERENT size. Spanning 0..70, 0..30, 0..25 before it moves.
constexpr double kBx = 70.0;
constexpr double kBy = 30.0;
constexpr double kBz = 25.0;
constexpr double kSecondVolume = kBx * kBy * kBz;

/// Two overlapping boxes of different sizes, the second shifted along X.
///
/// Different sizes on purpose, and not merely for variety: identical bodies exercise the naming
/// collision that the last test in this file guards. Keeping that case in ONE test means the rest
/// report the thing each is about rather than all reporting the same bug at once.
///
/// Built by MOVING one through the Translate command rather than constructing a primitive
/// elsewhere, because two coincident solids are a degenerate case that would prove nothing about
/// the ordinary one.
std::pair<ObjectId, ObjectId> twoOverlappingBoxes(Controller& app, double shift) {
    const auto first = aBox(app);

    REQUIRE(app.beginCommand("feature.box"));
    REQUIRE(app.setCommandParameter("dx", std::to_string(kBx)));
    REQUIRE(app.setCommandParameter("dy", std::to_string(kBy)));
    REQUIRE(app.setCommandParameter("dz", std::to_string(kBz)));
    REQUIRE(app.commitCommand());
    app.refresh();
    REQUIRE(app.selection().size() == 1);
    const auto second = app.selection().front();

    app.select(second, /*additive=*/false);
    REQUIRE(app.beginCommand("feature.translate"));
    REQUIRE(app.setCommandParameter("dx", std::to_string(shift)));
    REQUIRE(app.commitCommand());
    app.refresh();
    REQUIRE(app.selection().size() == 1);
    const auto moved = app.selection().front();
    REQUIRE(errorOf(app, moved) == "");

    return {first, moved};
}

/// The shared volume of the two boxes when the second is shifted by `shift` along X.
double overlapVolume(double shift) {
    const double x = std::max(0.0, std::min(kDx, shift + kBx) - std::max(0.0, shift));
    return x * std::min(kDy, kBy) * std::min(kDz, kBz);
}

/// Selects exactly two bodies, which is what every boolean here requires.
void selectBoth(Controller& app, ObjectId a, ObjectId b) {
    app.select(a, /*additive=*/false);
    app.select(b, /*additive=*/true);
    REQUIRE(app.selection().size() == 2);
}

}  // namespace

TEST_CASE("Join fuses two bodies from the catalogue", "[boolean][command]") {
    Controller app;
    const double shift = 50.0;
    auto [a, b] = twoOverlappingBoxes(app, shift);
    selectBoth(app, a, b);

    const auto fused = invokeCommand(app, "feature.fuse");
    CHECK(errorOf(app, fused) == "");

    // Inclusion-exclusion, not "bigger than one of them": a fuse that silently returned just one
    // of its inputs would pass a >= assertion and be completely wrong.
    CHECK(volumeOf(app, fused)
          == Approx(kBoxVolume + kSecondVolume - overlapVolume(shift)).epsilon(0.001));
}

TEST_CASE("Intersect keeps only the shared volume", "[boolean][command]") {
    // `booleanCommon` had no test on ANY path before this one -- not through the catalogue, and not
    // by direct call either. It was the single least-covered operation in the kernel.
    Controller app;
    const double shift = 50.0;
    auto [a, b] = twoOverlappingBoxes(app, shift);
    selectBoth(app, a, b);

    const auto common = invokeCommand(app, "feature.common");
    CHECK(errorOf(app, common) == "");

    CHECK(volumeOf(app, common) == Approx(overlapVolume(shift)).epsilon(0.001));
}

TEST_CASE("Intersect of disjoint bodies does not silently succeed", "[boolean][command]") {
    // Moved clear of each other, so the intersection is empty. What must NOT happen is a feature
    // that reports success and hands back one of its inputs -- the failure mode that makes a user
    // believe two parts touch when they do not.
    Controller app;
    auto [a, b] = twoOverlappingBoxes(app, kDx * 2);
    selectBoth(app, a, b);

    const auto* command = commandNamed(app, "feature.common");
    REQUIRE(command != nullptr);
    REQUIRE(command->enabled(app.context()));
    command->invoke();
    app.refresh();

    const auto common = app.selection().front();
    const double volume = volumeOf(app, common);
    // Either it refuses, or it produces genuinely nothing. Both are honest; a full-sized body is
    // not, and that is the whole assertion.
    CHECK(volume < 0.5 * kBoxVolume);
}

TEST_CASE("Chamfer is reachable with a whole body selected", "[chamfer][command]") {
    // Chamfer declares TWO acceptable selections -- some edges, or one body -- and the body form is
    // the one no test drove. An alternative that is declared and never exercised is exactly where
    // an enable predicate drifts away from what the feature accepts.
    Controller app;
    const auto box = aBox(app);
    app.select(box, /*additive=*/false);

    const auto chamfered = invokeCommand(app, "feature.chamfer");
    CHECK(errorOf(app, chamfered) == "");

    // Bevelling every edge removes material and cannot add any.
    const double after = volumeOf(app, chamfered);
    REQUIRE(after > 0.0);
    CHECK(after < kBoxVolume);
}

TEST_CASE("a boolean needs two bodies, and says so", "[boolean][command]") {
    // The refusal half. A command that lights up with one body selected and then fails is worse
    // than one that stays dim, because the failure arrives after a feature is already in the tree.
    Controller app;
    const auto box = aBox(app);
    app.select(box, /*additive=*/false);

    for (const auto* id : {"feature.fuse", "feature.common", "feature.cut"}) {
        const auto* command = commandNamed(app, id);
        REQUIRE(command != nullptr);
        INFO("command: " << id);
        CHECK_FALSE(command->enabled(app.context()));
    }
}

TEST_CASE("a boolean between two IDENTICAL bodies", "[boolean][command]") {
    // The regression guard for a bug that IS fixed, which is why this is an ordinary test now.
    //
    // Two boxes of the same dimensions used to fail every boolean with "Two pieces of this shape
    // ended up with the same identity" -- not about coplanar faces (offsetting along all three axes
    // failed identically) and not about booleans in general (two DIFFERENTLY sized boxes worked).
    // The trigger was the bodies being geometrically indistinguishable, so their elements measured
    // the same and the naming could not tell one body's face from the other's.
    //
    // Fixed by unifying the coplanar walls a fuse leaves behind, BEFORE naming runs on the result.
    //
    // It was tagged `!mayfail` while the bug was open. That tag accepts either outcome, so leaving
    // it here once the bug was fixed would mean the collision could come back and the suite would
    // stay green -- the tag has to come off the moment the bug does.
    //
    // Duplicating a part and joining it is ordinary modelling: two identical brackets, a mirrored
    // half welded to its original.
    Controller app;
    const auto first = aBox(app);
    const auto second = aBox(app);

    app.select(second, /*additive=*/false);
    REQUIRE(app.beginCommand("feature.translate"));
    REQUIRE(app.setCommandParameter("dx", "50"));
    REQUIRE(app.commitCommand());
    app.refresh();
    const auto moved = app.selection().front();

    selectBoth(app, first, moved);
    const auto fused = invokeCommand(app, "feature.fuse");
    INFO("error: " << errorOf(app, fused));
    CHECK(errorOf(app, fused) == "");
}
