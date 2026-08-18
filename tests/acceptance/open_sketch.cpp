/// A sketch is valid geometry on its own, closed or not.
///
/// # The bug this pins down
///
/// `computeSketch` used to call `toFace()` and fail whatever it returned. `toFace` refuses geometry
/// that is not closed and connected — which is correct for building a solid and wrong as the
/// definition of a sketch. The moment a user drew a single line, or added one line to the seeded
/// rectangle, the feature failed with "The sketch profile is not connected" and the model tree
/// showed ERR against the thing they were in the middle of drawing.
///
/// The closed-profile requirement belongs to whatever CONSUMES the sketch. These tests hold both
/// halves of that: the sketch computes, and the extrude is the one that complains.

#include "cad/app/Controller.h"

#include <catch2/catch_test_macros.hpp>

using namespace cad;

TEST_CASE("a sketch with open curves computes", "[sketch][open]") {
    app::Controller controller;
    controller.setViewportSize(1000, 800);

    const document::ObjectId id = controller.beginSketch();
    REQUIRE(id != document::ObjectId{});
    controller.alignCameraToSketch();
    controller.setSketchTool(app::Controller::SketchTool::Line);

    // One line across the seeded rectangle, which is exactly what a user does first and exactly
    // what used to break it: the profile is no longer a single closed loop.
    REQUIRE(controller.sketchClickAt(300.0f, 300.0f));
    REQUIRE(controller.sketchClickAt(700.0f, 500.0f));
    controller.finishSketch();

    const auto sketch = controller.document().find(id);
    REQUIRE(sketch != nullptr);

    // Computed, with output. Asserted as the STATE and not merely as "no exception", because the
    // failure mode was a feature that returned cleanly and carried an error the tree rendered.
    CHECK(sketch->state() == document::ObjectState::Clean);
    CHECK(sketch->error().message.empty());
    CHECK(sketch->output() != nullptr);
}

TEST_CASE("a closed sketch still produces a face", "[sketch][open]") {
    app::Controller controller;
    controller.setViewportSize(1000, 800);

    // The seeded sketch is a closed 40 x 25 rectangle and must keep giving a FACE — the fallback
    // to loose curves must not swallow the case that works, or every extrude in the application
    // would quietly stop building solids.
    const document::ObjectId id = controller.beginSketch();
    controller.finishSketch();

    const auto sketch = controller.document().find(id);
    REQUIRE(sketch != nullptr);
    REQUIRE(sketch->output() != nullptr);
    CHECK(sketch->output()->shape.type() == kernel::ShapeType::Face);
}

TEST_CASE("extruding open curves fails against the extrude, not the sketch", "[sketch][open]") {
    app::Controller controller;
    controller.setViewportSize(1000, 800);

    const document::ObjectId sketchId = controller.beginSketch();
    controller.alignCameraToSketch();
    controller.setSketchTool(app::Controller::SketchTool::Line);
    REQUIRE(controller.sketchClickAt(300.0f, 300.0f));
    REQUIRE(controller.sketchClickAt(700.0f, 500.0f));
    controller.finishSketch();

    // Through the command surface, as the ribbon does — addExtrude itself is private, and driving
    // the public path also proves the command is reachable with a sketch selected.
    REQUIRE(controller.beginCommand("feature.extrude"));
    REQUIRE(controller.commitCommand());

    // The sketch is fine; the extrude is the one that cannot proceed. That is the whole point of
    // moving the requirement: the complaint appears against the operation the user just asked for,
    // naming what it needs, instead of against the drawing they were happy with.
    const auto sketch = controller.document().find(sketchId);
    REQUIRE(sketch != nullptr);
    CHECK(sketch->state() == document::ObjectState::Clean);

    bool sawExtrude = false;
    for (const auto& item : controller.tree()) {
        const auto object = controller.document().find(item.id);
        if (!object || object->type() != "Extrude") continue;
        sawExtrude = true;
        CHECK(object->state() != document::ObjectState::Clean);
        CHECK(object->error().message.find("closed profile") != std::string::npos);
    }
    CHECK(sawExtrude);
}
