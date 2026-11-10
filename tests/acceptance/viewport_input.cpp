/// The viewport's pointer rules, now that they live in one place (app/ViewportInput).
///
/// Each rule here was a bug in a shell before it was a rule, and each shell had to remember it.
/// These drive ViewportInput the way a shell does -- press, move, release, in logical pixels --
/// and check WHICH rule answered, not what a GPU picked: a headless Controller has no id buffer,
/// so a click is a Selected outcome that found nothing, which is exactly enough to tell a click
/// from a drag.

#include "cad/app/Controller.h"
#include "cad/app/ViewportInput.h"

#include <catch2/catch_test_macros.hpp>

using namespace cad;
using app::ViewportInput;
using Action = ViewportInput::Action;
using Button = ViewportInput::Button;

namespace {

ViewportInput::Pointer at(float x, float y) { return ViewportInput::Pointer{x, y}; }

void invoke(app::Controller& controller, const std::string& id) {
    for (const auto& c : controller.commands())
        if (c.id == id) c.invoke();
}

}  // namespace

TEST_CASE("a drag past the threshold is navigation, not a click", "[viewport][input]") {
    app::Controller controller;
    controller.setViewportSize(800, 600);
    ViewportInput input(controller);
    controller.setOrbitMode(true);  // a plain left drag orbits

    input.press(Button::Primary, at(100, 100));
    CHECK(input.drag() == render::Drag::Orbit);
    CHECK(input.move(at(130, 100)).action == Action::Navigated);
    // Released after moving: it was a gesture, and must not ALSO select what is under the pointer.
    CHECK(input.release(Button::Primary, at(130, 100)).action == Action::None);
}

TEST_CASE("a shaky click is still a click", "[viewport][input]") {
    app::Controller controller;
    controller.setViewportSize(800, 600);
    ViewportInput input(controller);
    controller.setOrbitMode(true);

    input.press(Button::Primary, at(100, 100));
    input.move(at(102, 101));  // three logical pixels: under the four-pixel threshold
    CHECK(input.release(Button::Primary, at(102, 101)).action == Action::Selected);
}

TEST_CASE("the release after a double click is swallowed", "[viewport][input]") {
    app::Controller controller;
    controller.setViewportSize(800, 600);
    ViewportInput input(controller);

    input.press(Button::Primary, at(50, 50));
    CHECK(input.release(Button::Primary, at(50, 50)).action == Action::Selected);
    // The double click selects the body; the release that follows must not re-select at the
    // current level, replacing the body with the face under the pointer.
    CHECK(input.doubleClick(Button::Primary, at(50, 50)).action == Action::Selected);
    CHECK(input.release(Button::Primary, at(50, 50)).action == Action::None);
}

TEST_CASE("a swallowed release cannot outlive its gesture", "[viewport][input]") {
    app::Controller controller;
    controller.setViewportSize(800, 600);
    ViewportInput input(controller);

    // libgui's order: the double click arrives with no release after it in this shell's stream,
    // so the flag is still set when the NEXT gesture begins. That gesture's click must count.
    input.doubleClick(Button::Primary, at(50, 50));
    input.press(Button::Primary, at(60, 60));
    CHECK(input.release(Button::Primary, at(60, 60)).action == Action::Selected);
}

TEST_CASE("in a sketch the left press draws, unless orbit mode is on", "[viewport][input]") {
    app::Controller controller;
    controller.setViewportSize(800, 600);
    REQUIRE(controller.beginSketchOn(cad::sketch::Plane::XY) != document::ObjectId{});
    controller.setSketchTool(app::Controller::SketchTool::Line);
    ViewportInput input(controller);

    CHECK(input.press(Button::Primary, at(200, 200)).action == Action::Drew);
    input.release(Button::Primary, at(200, 200));

    controller.setOrbitMode(true);
    CHECK(input.press(Button::Primary, at(200, 200)).action != Action::Drew);
    CHECK(input.drag() == render::Drag::Orbit);
}

TEST_CASE("alt keeps navigation available while a tool is active", "[viewport][input]") {
    app::Controller controller;
    controller.setViewportSize(800, 600);
    REQUIRE(controller.beginSketchOn(cad::sketch::Plane::XY) != document::ObjectId{});
    controller.setSketchTool(app::Controller::SketchTool::Line);
    ViewportInput input(controller);

    ViewportInput::Pointer p = at(200, 200);
    p.alt = true;
    CHECK(input.press(Button::Primary, p).action != Action::Drew);
}

TEST_CASE("a click waiting for a sketch plane picks the plane", "[viewport][input]") {
    app::Controller controller;
    controller.setViewportSize(800, 600);
    // Start Sketch with nothing selected asks for a plane rather than guessing one.
    invoke(controller, "feature.sketch");
    REQUIRE(controller.awaitingSketchPlane());
    ViewportInput input(controller);

    input.press(Button::Primary, at(400, 300));
    CHECK(input.release(Button::Primary, at(400, 300)).action == Action::PickedPlane);
}

TEST_CASE("only the primary button clicks", "[viewport][input]") {
    app::Controller controller;
    controller.setViewportSize(800, 600);
    ViewportInput input(controller);

    input.press(Button::Secondary, at(10, 10));
    CHECK(input.release(Button::Secondary, at(10, 10)).action == Action::None);
}
