/// Start Sketch, draw, Finish Sketch — the shape of the whole interaction.
///
/// These are workflow tests rather than unit tests, and they exist because every bug reported
/// against in-place sketching so far has been a WORKFLOW bug that the piecewise tests could not
/// see: the pixels drew correctly, the mapping was right, the overlay was right, and finishing the
/// sketch still failed. A test that never finishes a sketch never touches the code that failed.

#include "cad/app/Controller.h"
#include "cad/kernel/Shape.h"

#include <cmath>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using namespace cad;

namespace {

/// How closely the camera looks along `axis`. 1.0 is dead-on.
double alignmentWith(app::Controller& c, const std::array<double, 3>& axis) {
    const auto basis = c.camera().basis();
    return std::abs(basis.forward[0] * axis[0] + basis.forward[1] * axis[1] +
                    basis.forward[2] * axis[2]);
}

}  // namespace

TEST_CASE("Start Sketch uses the selected face, and Finish gives the view back", "[sketch][flow]") {
    app::Controller controller;
    controller.setViewportSize(1200, 800);

    REQUIRE(controller.beginCommand("feature.box"));
    REQUIRE(controller.commitCommand());
    // Found by TYPE, not by position. A new part now starts with three origin datum planes, so
    // the first tree row is a plane and "the box is row zero" was only ever true by accident.
    document::ObjectId boxId{};
    for (const auto& item : controller.tree()) {
        const auto object = controller.document().find(item.id);
        if (object && object->type() == "Box") { boxId = item.id; break; }
    }
    REQUIRE(boxId != document::ObjectId{});

    // An off-axis view the user arranged for themselves. Finishing must give exactly this back —
    // a sketch that quietly re-aims the camera makes every sketch cost a re-orbit.
    controller.camera().orbit(34.0f, 21.0f);
    const auto arranged = controller.camera().basis();

    // Select a side face, the way a click does.
    const auto box = controller.document().find(boxId);
    REQUIRE(box != nullptr);
    REQUIRE(box->output() != nullptr);
    std::array<double, 3> faceNormal{};
    bool selected = false;
    for (const auto& name : box->output()->map.allNames()) {
        const auto shape = box->output()->map.resolve(name);
        if (!shape) continue;
        const auto plane = kernel::planeOf(*shape);
        if (!plane) continue;
        const auto& f = plane.value();
        const double nx = f.u[1] * f.v[2] - f.u[2] * f.v[1];
        const double ny = f.u[2] * f.v[0] - f.u[0] * f.v[2];
        const double nz = f.u[0] * f.v[1] - f.u[1] * f.v[0];
        // A face looking along X: NOT the XY plane a default sketch would land on, so an
        // implementation that ignored the selection gives a visibly different answer.
        if (std::abs(nx) > 0.9) {
            controller.setSelectionLevel(app::Controller::SelectionLevel::Face);
            REQUIRE(controller.selectElement(boxId, name));
            faceNormal = {nx, ny, nz};
            selected = true;
            break;
        }
    }
    REQUIRE(selected);

    const document::ObjectId sketchId = controller.beginSketch();
    REQUIRE(sketchId != document::ObjectId{});
    REQUIRE(controller.environment() == app::Environment::Sketch);

    // The camera went to the SELECTED face, not to XY.
    CHECK_THAT(alignmentWith(controller, faceNormal),
               Catch::Matchers::WithinAbs(1.0, 1e-3));

    // And a click lands on that sketch, which is the point of aiming the camera at it.
    CHECK(controller.sketchPointAt(600.0f, 400.0f).has_value());

    controller.setSketchTool(app::Controller::SketchTool::Line);
    REQUIRE(controller.sketchClickAt(500.0f, 350.0f));
    REQUIRE(controller.sketchClickAt(700.0f, 450.0f));
    controller.finishSketch();

    REQUIRE(controller.environment() == app::Environment::Model);

    // The arranged view is BACK, to the same three axes it had before the sketch opened.
    const auto after = controller.camera().basis();
    for (int i = 0; i < 3; ++i) {
        CHECK_THAT(static_cast<double>(after.forward[i]),
                   Catch::Matchers::WithinAbs(static_cast<double>(arranged.forward[i]), 1e-4));
        CHECK_THAT(static_cast<double>(after.up[i]),
                   Catch::Matchers::WithinAbs(static_cast<double>(arranged.up[i]), 1e-4));
    }

    // And the sketch itself computed — no ERR on the thing the user just drew.
    const auto sketch = controller.document().find(sketchId);
    REQUIRE(sketch != nullptr);
    CHECK(sketch->state() == document::ObjectState::Clean);
}

TEST_CASE("cancelling a sketch also gives the view back", "[sketch][flow]") {
    app::Controller controller;
    controller.setViewportSize(1000, 800);
    controller.camera().orbit(12.0f, 40.0f);
    const auto arranged = controller.camera().basis();

    REQUIRE(controller.beginSketch() != document::ObjectId{});
    controller.cancelSketch();

    // Abandoning a sketch must cost nothing, including the view. Restoring only on Finish would
    // punish the user for changing their mind.
    const auto after = controller.camera().basis();
    for (int i = 0; i < 3; ++i) {
        CHECK_THAT(static_cast<double>(after.forward[i]),
                   Catch::Matchers::WithinAbs(static_cast<double>(arranged.forward[i]), 1e-4));
    }
}

TEST_CASE("entering a sketch turns orbit mode off", "[sketch][flow]") {
    app::Controller controller;
    controller.setViewportSize(1000, 800);
    controller.setOrbitMode(true);

    REQUIRE(controller.beginSketch() != document::ObjectId{});
    controller.setSketchTool(app::Controller::SketchTool::Line);

    // Orbit left on means the first stroke rotates the model instead of drawing, which reads as
    // the sketch tools being broken. Entering a sketch is the moment the user asked to DRAW.
    CHECK_FALSE(controller.orbitMode());
    CHECK(controller.leftPressDraws());
}

TEST_CASE("a new part has three origin planes you can sketch on", "[sketch][flow][datum]") {
    app::Controller controller;
    controller.setViewportSize(1000, 800);

    // Three datums, in the document, before any geometry exists. This is the whole point: without
    // them Start Sketch has nothing to select on an empty part and has to GUESS a plane — and a
    // guessed plane is invisible, so the user cannot tell which one they got.
    int planes = 0;
    document::ObjectId xz{};
    for (const auto& item : controller.tree()) {
        const auto object = controller.document().find(item.id);
        if (!object || object->type() != "Plane") continue;
        ++planes;
        // Computed, not merely declared: a datum that failed to build cannot be sketched on, and
        // the failure would only surface at the moment the user tried.
        CHECK(object->state() == document::ObjectState::Clean);
        REQUIRE(object->output() != nullptr);
        if (object->label().find("XZ") != std::string::npos) xz = item.id;
    }
    CHECK(planes == 3);
    REQUIRE(xz != document::ObjectId{});

    // Selecting the XZ datum and starting a sketch puts the camera on THAT plane — XZ's normal is
    // Y, so an implementation that ignored the selection and used XY would look along Z instead.
    const auto plane = controller.document().find(xz);
    // The FACE, not simply the first name: a map holds the datum's edges and vertices too, and
    // allNames() has no defined order.
    naming::ElementName faceName;
    bool found = false;
    for (const auto& name : plane->output()->map.allNames()) {
        const auto shape = plane->output()->map.resolve(name);
        if (shape && kernel::planeOf(*shape)) { faceName = name; found = true; break; }
    }
    REQUIRE(found);
    controller.setSelectionLevel(app::Controller::SelectionLevel::Face);
    REQUIRE(controller.selectElement(xz, faceName));

    const document::ObjectId sketchId = controller.beginSketch();
    REQUIRE(sketchId != document::ObjectId{});
    REQUIRE(controller.environment() == app::Environment::Sketch);
    CHECK_THAT(alignmentWith(controller, {0.0, 1.0, 0.0}),
               Catch::Matchers::WithinAbs(1.0, 1e-3));

    // And it is usable: a click lands, a line draws, finishing does not error.
    controller.setSketchTool(app::Controller::SketchTool::Line);
    REQUIRE(controller.sketchClickAt(400.0f, 350.0f));
    REQUIRE(controller.sketchClickAt(600.0f, 450.0f));
    controller.finishSketch();

    const auto sketch = controller.document().find(sketchId);
    REQUIRE(sketch != nullptr);
    CHECK(sketch->state() == document::ObjectState::Clean);
}

TEST_CASE("origin planes are reference geometry, not bodies", "[sketch][flow][datum]") {
    app::Controller controller;
    controller.setViewportSize(1000, 800);

    // Hidden by default. Shown, three large sheets sit in front of the model, get picked before the
    // faces behind them, and "fit" frames the datums rather than the part. Hidden is not absent:
    // they are in the tree and the test above sketches on one.
    CHECK(controller.stats().instances == 0);

    REQUIRE(controller.beginCommand("feature.box"));
    REQUIRE(controller.commitCommand());

    // One body drawn, not four. The datums must not count as bodies for drawing, for framing, or
    // for the tip-body rule that decides what a feature consumed.
    CHECK(controller.stats().instances == 1);
}

TEST_CASE("datums are reference geometry, not history", "[sketch][flow][datum]") {
    app::Controller controller;
    controller.setViewportSize(1000, 800);
    REQUIRE(controller.beginCommand("feature.box"));
    REQUIRE(controller.commitCommand());

    int origin = 0;
    int history = 0;
    for (const auto& item : controller.tree()) {
        const auto object = controller.document().find(item.id);
        REQUIRE(object != nullptr);
        if (item.group == app::TreeGroup::Origin) {
            ++origin;
            // Only datums. A body landing in the Origin group would vanish from the history a
            // user scrubs, which is worse than the flat list this replaces.
            CHECK(object->type() == "Plane");
        } else {
            ++history;
            CHECK(object->type() != "Plane");
        }
    }
    CHECK(origin == 3);
    CHECK(history == 1);   // the box, and nothing else pretending to be a modelling step
}

TEST_CASE("a plane picked in the tree is the plane you sketch on", "[sketch][flow][datum]") {
    // The tree selects OBJECTS; the viewport selects ELEMENTS. Start Sketch has to honour both, or
    // choosing "XZ Plane" in the browser and pressing Start Sketch gives an XY sketch — silently,
    // and only visibly wrong once you have drawn on it.
    app::Controller controller;
    controller.setViewportSize(1000, 800);

    document::ObjectId xz{};
    for (const auto& item : controller.tree()) {
        const auto object = controller.document().find(item.id);
        if (object && object->type() == "Plane" &&
            object->label().find("XZ") != std::string::npos) {
            xz = item.id;
        }
    }
    REQUIRE(xz != document::ObjectId{});

    controller.select(xz, false);          // exactly what clicking the tree row does
    REQUIRE(controller.beginSketch() != document::ObjectId{});

    // XZ's normal is Y. The XY fallback would look along Z, so the two answers disagree.
    CHECK_THAT(alignmentWith(controller, {0.0, 1.0, 0.0}),
               Catch::Matchers::WithinAbs(1.0, 1e-3));
}

TEST_CASE("Slice cuts between the viewer and the sketch plane", "[sketch][flow][slice]") {
    app::Controller controller;
    controller.setViewportSize(1000, 800);
    REQUIRE(controller.beginSketch() != document::ObjectId{});
    controller.alignCameraToSketch();

    // Off on entry: a part arriving half-missing because the last sketch left Slice on is alarming
    // rather than helpful.
    CHECK_FALSE(controller.sliceEnabled());
    CHECK(controller.frame().sections.empty());

    controller.setSliceEnabled(true);
    REQUIRE(controller.frame().sections.size() == 1);

    const auto facingCamera = [&] {
        const auto& plane = controller.frame().sections.front();
        const auto basis = controller.camera().basis();
        // The normal must point back towards the eye: the cut removes what is on the camera's side.
        return -(plane.normal[0] * basis.forward[0] + plane.normal[1] * basis.forward[1] +
                 plane.normal[2] * basis.forward[2]);
    };
    CHECK(facingCamera() > 0.0f);

    // Orbiting to the other side has to FLIP it. Without that, orbiting past the plane shows the
    // half just cut away and hides the half being worked on — the tool would appear to work at
    // random, which is worse than not having it.
    controller.camera().orbit(0.0f, 400.0f);
    controller.cameraChanged();
    REQUIRE(controller.frame().sections.size() == 1);
    CHECK(facingCamera() > 0.0f);

    // Sketch-scoped: leaving takes the cut with it.
    controller.finishSketch();
    CHECK_FALSE(controller.sliceEnabled());
    CHECK(controller.frame().sections.empty());
}
