/// Starting a sketch, and what the view does while one is open.
///
/// Both rules here are about NOT guessing on the user's behalf:
///
///   * Start Sketch with nothing selected asks which plane, rather than silently choosing XY.
///     "What none of them do is silently choose a plane for you" — docs/design/MODELLING_UX.md §2.
///     A guessed plane is invisible, so the user finds out when the extrude comes out facing the
///     wrong way.
///   * The view stays normal to the sketch plane until the sketch is finished. This one is a
///     DELIBERATE DIVERGENCE — Fusion, SolidWorks and Inventor all allow orbiting inside a sketch —
///     and it is tested so nobody restores the incumbents' behaviour by accident.

#include "cad/app/Controller.h"

#include <catch2/catch_test_macros.hpp>

using namespace cad;

TEST_CASE("Start Sketch with nothing selected asks for a plane", "[sketch][entry]") {
    app::Controller c;
    const auto id = c.beginSketch();

    // No sketch, no environment change — a question, not a guess.
    CHECK(id == document::ObjectId{});
    CHECK(c.awaitingSketchPlane());
    CHECK(c.environment() == app::Environment::Model);
    CHECK(c.activeSketch() == nullptr);
}

TEST_CASE("a selected plane still opens a sketch directly", "[sketch][entry]") {
    // Asking is the FALLBACK, not the flow. Selecting a plane and pressing Sketch must still work
    // in one step, which is what every CAD application does and what makes the ask tolerable.
    app::Controller c;
    document::ObjectId plane;
    for (const auto& item : c.tree()) {
        if (item.type == "Plane") { plane = item.id; break; }
    }
    REQUIRE(plane != document::ObjectId{});

    c.select(plane, false);
    const auto id = c.beginSketch();
    CHECK(id != document::ObjectId{});
    CHECK_FALSE(c.awaitingSketchPlane());
    CHECK(c.environment() == app::Environment::Sketch);
}

TEST_CASE("the plane pick can be abandoned", "[sketch][entry]") {
    app::Controller c;
    REQUIRE(c.beginSketch() == document::ObjectId{});
    REQUIRE(c.awaitingSketchPlane());

    c.cancelSketchPlanePick();
    CHECK_FALSE(c.awaitingSketchPlane());
    // And a click that arrives afterwards is an ordinary one again.
    CHECK(c.sketchOnPickedPlane(100, 100) == document::ObjectId{});
}

TEST_CASE("the view is locked to the plane while a sketch is open", "[sketch][entry]") {
    app::Controller c;
    document::ObjectId plane;
    for (const auto& item : c.tree()) {
        if (item.type == "Plane") { plane = item.id; break; }
    }
    REQUIRE(plane != document::ObjectId{});

    // Outside a sketch, orbiting works.
    CHECK(c.orbitCamera(20.0f, 10.0f));

    c.select(plane, false);
    REQUIRE(c.beginSketch() != document::ObjectId{});
    const auto locked = c.camera().matrices(cad::render::Viewport{800, 600, 1.0f});

    // Inside one it is refused — and the camera does not move, which is the part that matters:
    // the pointer is unprojected onto the plane, so a view that drifted would leave every click
    // landing somewhere other than where it was aimed.
    CHECK_FALSE(c.orbitCamera(40.0f, 25.0f));
    const auto after = c.camera().matrices(cad::render::Viewport{800, 600, 1.0f});
    for (int i = 0; i < 16; ++i) {
        CHECK(after.view.m[i] == locked.view.m[i]);
    }

    // Finishing gives it back.
    c.finishSketch();
    CHECK(c.orbitCamera(10.0f, 5.0f));
}
