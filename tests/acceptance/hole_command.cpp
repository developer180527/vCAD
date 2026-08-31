// Drilling a hole, from the command catalogue.
//
// The Hole feature computed correctly for days and no user could reach it: nothing added it to
// `Controller`'s command catalogue, and BOTH shells build their tools from that catalogue by
// design. So the gap was not in the geometry, the naming, or the UI — it was one missing
// registration, which is the cheapest kind of gap and the hardest to see from inside the code that
// works.
//
// These tests drive the command the way a shell does — find it by id, check `enabled`, invoke it —
// rather than calling `addHole` directly. Calling the method would prove the geometry works, which
// was never in doubt; going through the catalogue is what proves a user can get to it.

#include "cad/app/Controller.h"
#include "cad/kernel/Shape.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <numbers>
#include <string>

using cad::app::Controller;
using Level = Controller::SelectionLevel;
using Catch::Approx;

namespace {

const cad::app::Command* commandNamed(const Controller& app, const std::string& id) {
    for (const auto& command : app.commands()) {
        if (command.id == id) return &command;
    }
    return nullptr;
}

/// Volume of an object's computed shape, or -1 if it has none.
double volumeOf(const Controller& app, cad::document::ObjectId id) {
    const auto object = app.document().find(id);
    if (!object || object->output() == nullptr) return -1.0;
    return object->output()->shape.volume();
}

/// Selects a face of `id` at `level` Face, and returns whether one was found.
///
/// Walks the scene's slots through the same pick path a click uses, so what gets selected is what a
/// user could have selected.
bool selectAFace(Controller& app, cad::document::ObjectId id) {
    app.setSelectionLevel(Level::Face);
    for (std::uint32_t slot = 0; slot < 128; ++slot) {
        app.scriptNextPick(slot);
        const auto pick = app.pickAt(10, 10);
        if (!pick.hit || pick.object != id) continue;
        app.scriptNextPick(slot);
        if (app.clickAt(10, 10, /*additive=*/false).changed) return true;
    }
    return false;
}

cad::document::ObjectId aBox(Controller& app) {
    const auto* box = commandNamed(app, "feature.box");
    REQUIRE(box != nullptr);
    box->invoke();
    app.refresh();
    REQUIRE(app.selection().size() == 1);
    return app.selection().front();
}

}  // namespace

TEST_CASE("Hole is in the command catalogue at all", "[hole][command]") {
    // The whole bug, in one assertion. Both shells enumerate this catalogue; a feature missing from
    // it does not exist as far as anyone using the application is concerned.
    Controller app;
    CHECK(commandNamed(app, "feature.hole") != nullptr);
}

TEST_CASE("Hole offers itself only when a face is selected", "[hole][command]") {
    Controller app;
    const auto id = aBox(app);
    const auto* hole = commandNamed(app, "feature.hole");
    REQUIRE(hole != nullptr);

    // A body selected is not a face selected. The feature takes its position AND its direction from
    // one flat face, so a body says nothing about where to drill.
    CHECK_FALSE(hole->enabled(app.context()));

    REQUIRE(selectAFace(app, id));
    CHECK(hole->enabled(app.context()));

    // And an edge is not a face either.
    app.setSelectionLevel(Level::Edge);
    CHECK_FALSE(hole->enabled(app.context()));
}

TEST_CASE("Hole removes material", "[hole][command]") {
    // Asserted as a VOLUME, not as "a feature was added". A hole drilled in the wrong direction
    // cuts nothing at all and reports success — the failure computeHole's inward-normal test exists
    // to prevent — and a test that counted features would pass through it happily.
    Controller app;
    const auto id = aBox(app);
    const double before = volumeOf(app, id);
    REQUIRE(before > 0.0);

    REQUIRE(selectAFace(app, id));
    const auto* hole = commandNamed(app, "feature.hole");
    REQUIRE(hole != nullptr);
    REQUIRE(hole->enabled(app.context()));
    hole->invoke();
    app.refresh();

    REQUIRE(app.selection().size() == 1);
    const auto holeId = app.selection().front();
    const double after = volumeOf(app, holeId);
    REQUIRE(after > 0.0);

    // 8 mm across and 10 mm deep, so a cylinder of pi * 4^2 * 10. The box is 100 x 60 x 40 and the
    // hole is well inside every face of it, so the whole cylinder lands in material whichever face
    // was picked.
    const double expected = std::numbers::pi * 4.0 * 4.0 * 10.0;
    CHECK(after == Approx(before - expected).epsilon(0.02));
}

TEST_CASE("the drilled face is not left selected", "[hole][command]") {
    // The face belonged to the body the hole consumed. Leaving it selected would mark geometry that
    // no longer exists, and hand the NEXT command a reference into a superseded feature.
    Controller app;
    const auto id = aBox(app);
    REQUIRE(selectAFace(app, id));
    REQUIRE(app.elementSelection().size() == 1);

    commandNamed(app, "feature.hole")->invoke();
    app.refresh();
    CHECK(app.elementSelection().empty());
}

TEST_CASE("a curved face is refused without leaving wreckage", "[hole][command]") {
    // computeHole would refuse a cylinder's side too, but only after the feature had been added —
    // leaving a failed row in the browser for the user to find and delete. Refusing in the shell
    // costs one call to the same kernel function and leaves the document untouched.
    Controller app;
    const auto* cylinder = commandNamed(app, "feature.cylinder");
    REQUIRE(cylinder != nullptr);
    cylinder->invoke();
    app.refresh();
    const auto id = app.selection().front();

    std::string lastStatus;
    app.onStatus([&lastStatus](const std::string& s) { lastStatus = s; });

    app.setSelectionLevel(Level::Face);
    bool sawRefusal = false;
    const std::size_t before = app.tree().size();
    for (std::uint32_t slot = 0; slot < 64; ++slot) {
        app.scriptNextPick(slot);
        const auto pick = app.pickAt(10, 10);
        if (!pick.hit || pick.object != id) continue;
        app.scriptNextPick(slot);
        if (!app.clickAt(10, 10, false).changed) continue;

        const auto object = app.document().find(id);
        const auto shape = object->output()->map.resolve(app.elementSelection().front().element);
        if (!shape || cad::kernel::planeOf(*shape)) continue;   // a flat cap: not this case

        // Through the command, like everything else here: `addHole` is private, and the point is
        // that the path a user takes refuses cleanly.
        commandNamed(app, "feature.hole")->invoke();
        sawRefusal = true;
        INFO("status: " << lastStatus);
        CHECK_FALSE(lastStatus.empty());
        CHECK(app.tree().size() == before);   // nothing was added
    }
    CHECK(sawRefusal);
}

TEST_CASE("the Hole panel opens with the selection the button accepted", "[hole][command]") {
    // The bug this closes. `enabled` counted what is SELECTED while `beginCommand` tested the
    // selection LEVEL, and Auto — the default — is neither Face nor Edge. So the button lit up, the
    // panel refused, and the shell fell back to invoking the command with its hard-coded 8 x 10 mm:
    // the feature appeared to work and its parameters were unreachable.
    Controller app;
    const auto id = aBox(app);

    // Auto, as a new document starts. Not Face — the point is that the level is not what decides.
    app.setSelectionLevel(Level::Auto);
    bool picked = false;
    for (std::uint32_t slot = 0; slot < 128 && !picked; ++slot) {
        app.scriptNextPick(slot);
        const auto pick = app.pickAt(10, 10);
        if (!pick.hit || pick.object != id) continue;
        app.scriptNextPick(slot);
        if (!app.clickAt(10, 10, /*additive=*/false).changed) continue;
        picked = app.context().selectedFaces == 1;
    }
    REQUIRE(picked);

    const auto* hole = commandNamed(app, "feature.hole");
    REQUIRE(hole != nullptr);
    REQUIRE(hole->enabled(app.context()));

    // Whatever the button accepts, the panel must accept — they ask one question now.
    REQUIRE(app.beginCommand("feature.hole"));
    const auto& parameters = app.commandParameters();
    REQUIRE(parameters.size() == 2);
    CHECK(parameters[0].name == "diameter");
    CHECK(parameters[1].name == "depth");
    app.cancelCommand();
}
