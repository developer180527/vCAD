/// Every navigation gesture is reachable on the hardware people actually have.
///
/// This file exists because orbit was UNREACHABLE on a laptop for the entire life of the viewport.
/// Every preset mapped it to the middle mouse button; a trackpad has none. Pan worked and zoom
/// worked, so the viewport looked alive while being impossible to rotate — and no test noticed,
/// because the gesture table was only ever checked one mapping at a time against itself.
///
/// The check below is therefore not "does this button do that". It is: for each preset, is every
/// gesture reachable using only buttons a trackpad has? That is a question about the table as a
/// whole, and it is the one that was never asked.

#include "cad/app/Controller.h"
#include "cad/render/Camera.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

using namespace cad;
using render::Drag;

TEST_CASE("every gesture is reachable without a middle button", "[camera][navigation]") {
    const auto preset = GENERATE(render::NavigationPreset::Cad, render::NavigationPreset::Blender,
                                 render::NavigationPreset::Fusion);
    render::CameraController camera;
    camera.setPreset(preset);

    constexpr int kLeft = 0, kRight = 2;

    // A trackpad offers left and right (two-finger) click, plus modifiers. Nothing else.
    bool orbit = false;
    bool pan = false;
    for (const int button : {kLeft, kRight}) {
        for (const bool shift : {false, true}) {
            for (const bool ctrl : {false, true}) {
                for (const bool alt : {false, true}) {
                    switch (camera.dragFor(button, shift, ctrl, alt)) {
                        case Drag::Orbit: orbit = true; break;
                        case Drag::Pan:   pan = true; break;
                        default: break;
                    }
                }
            }
        }
    }
    CHECK(orbit);
    CHECK(pan);
}

TEST_CASE("a plain left drag is still selection, never navigation", "[camera][navigation]") {
    const auto preset = GENERATE(render::NavigationPreset::Cad, render::NavigationPreset::Blender,
                                 render::NavigationPreset::Fusion);
    render::CameraController camera;
    camera.setPreset(preset);

    // The fix must not cost the click. An unmodified left drag that orbited would break selection,
    // which is the other half of what a viewport is for.
    CHECK(camera.dragFor(0, false, false, false) == Drag::None);
}

TEST_CASE("the middle button keeps working for people with a mouse", "[camera][navigation]") {
    render::CameraController camera;
    camera.setPreset(render::NavigationPreset::Cad);
    // The trackpad binding is additive. Someone on a three-button mouse must not have their muscle
    // memory changed to fix someone else's laptop.
    CHECK(camera.dragFor(1, false, false, false) == Drag::Orbit);
    CHECK(camera.dragFor(1, true, false, false) == Drag::Pan);
}

TEST_CASE("orbit mode beats the drawing tool", "[camera][navigation]") {
    // The bug this pins: the shell asked "is a sketch tool active?" BEFORE "is orbit mode on?", so
    // turning Orbit on while the Line tool was active did nothing — the press drew a point and the
    // mode was never consulted. Wrong ORDER is invisible in a chain of ifs, which is why the
    // precedence is a model rule with a test rather than shell code.
    app::Controller controller;
    controller.setViewportSize(800, 600);
    REQUIRE(controller.beginSketch() != document::ObjectId{});

    controller.setSketchTool(app::Controller::SketchTool::Line);
    CHECK(controller.leftPressDraws());

    controller.setOrbitMode(true);
    CHECK_FALSE(controller.leftPressDraws());   // the mode wins

    controller.setOrbitMode(false);
    CHECK(controller.leftPressDraws());

    // And with no tool active a left press never draws, orbit mode or not: Select means select.
    controller.setSketchTool(app::Controller::SketchTool::Select);
    CHECK_FALSE(controller.leftPressDraws());
}

TEST_CASE("outside a sketch a left press never draws", "[camera][navigation]") {
    app::Controller controller;
    controller.setViewportSize(800, 600);
    // No sketch open. The shell asks this on every press, so the answer must not depend on stale
    // tool state left over from the last sketch.
    controller.setSketchTool(app::Controller::SketchTool::Line);
    CHECK_FALSE(controller.leftPressDraws());
}
