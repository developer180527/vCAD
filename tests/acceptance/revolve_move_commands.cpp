// Revolve and Move, from the command catalogue.
//
// Both computed correctly and neither had a command, so no user could reach either — the gap the
// reachability guard now watches for. These tests are the other half of that guard: it proves a
// command EXISTS, and these prove the command does the thing.
//
// Driven through the catalogue rather than by calling `addRevolve` / `addTranslate`, because the
// methods were never the part that was missing.

#include "cad/app/Controller.h"
#include "cad/kernel/Shape.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
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

const cad::kernel::Shape* shapeOf(const Controller& app, cad::document::ObjectId id) {
    const auto object = app.document().find(id);
    if (!object || object->output() == nullptr) return nullptr;
    return &object->output()->shape;
}

cad::document::ObjectId aBox(Controller& app) {
    commandNamed(app, "feature.box")->invoke();
    app.refresh();
    REQUIRE(app.selection().size() == 1);
    return app.selection().front();
}

/// A FINISHED sketch holding a closed rectangle, offset from the origin.
///
/// Finished, not open: while a sketch is being edited its feature is deliberately not drawn (the
/// overlay draws it instead), so it has no elements in the scene and nothing to pick. That is
/// correct behaviour and it means a test wanting to select the sketch's own edges has to leave the
/// sketch environment first.
///
/// Offset in x so that revolving about any of its four edges sweeps a real solid rather than a
/// profile that crosses its own axis.
cad::document::ObjectId aSketch(Controller& app) {
    const auto id = app.beginSketchOn(cad::sketch::Plane::XY);
    REQUIRE(id != cad::document::ObjectId{});
    auto* sketch = app.activeSketch();
    REQUIRE(sketch != nullptr);

    const double x0 = 10.0, x1 = 30.0, y0 = 0.0, y1 = 20.0;
    sketch->addLine(x0, y0, x1, y0);
    sketch->addLine(x1, y0, x1, y1);
    sketch->addLine(x1, y1, x0, y1);
    sketch->addLine(x0, y1, x0, y0);

    app.finishSketch();
    app.refresh();
    return id;
}

/// Selects the first edge of `id` that a click at Edge level accepts, and reports whether it is
/// straight — a revolve axis has to be.
bool selectAnEdge(Controller& app, cad::document::ObjectId id) {
    app.setSelectionLevel(Level::Edge);
    for (std::uint32_t slot = 0; slot < 128; ++slot) {
        app.scriptNextPick(slot);
        const auto pick = app.pickAt(10, 10);
        if (!pick.hit || pick.object != id) continue;
        app.scriptNextPick(slot);
        if (!app.clickAt(10, 10, /*additive=*/false).changed) continue;

        const auto* shape = shapeOf(app, id);
        if (shape == nullptr) return false;
        const auto object = app.document().find(id);
        const auto edge = object->output()->map.resolve(app.elementSelection().front().element);
        if (edge && cad::kernel::lineOf(*edge)) return true;   // straight: usable as an axis
    }
    return false;
}

}  // namespace

TEST_CASE("Revolve turns a sketch into a solid", "[revolve][command]") {
    // Asserted as a VOLUME. The profile is a face — zero volume — so a revolve that silently did
    // nothing, or produced a sheet rather than a solid, would still leave a feature in the tree for
    // a test that only counted rows.
    Controller app;
    const auto sketch = aSketch(app);
    REQUIRE(shapeOf(app, sketch) != nullptr);
    CHECK(shapeOf(app, sketch)->volume() == Approx(0.0).margin(1e-9));

    REQUIRE(selectAnEdge(app, sketch));
    const auto* revolve = commandNamed(app, "feature.revolve");
    REQUIRE(revolve != nullptr);
    REQUIRE(revolve->enabled(app.context()));
    revolve->invoke();
    app.refresh();

    REQUIRE(app.selection().size() == 1);
    const auto* solid = shapeOf(app, app.selection().front());
    REQUIRE(solid != nullptr);
    CHECK(solid->volume() > 1.0);
}

TEST_CASE("Revolve offers itself only for an edge of a sketch", "[revolve][command]") {
    Controller app;
    const auto box = aBox(app);
    const auto* revolve = commandNamed(app, "feature.revolve");
    REQUIRE(revolve != nullptr);

    // A body is not an axis.
    CHECK_FALSE(revolve->enabled(app.context()));

    // Nor is an edge of a SOLID: computeRevolve resolves the axis in the profile's own element map,
    // so an edge of some other body cannot be found there — and a command that offered itself and
    // then failed would be worse than one that stayed grey.
    REQUIRE(selectAnEdge(app, box));
    CHECK_FALSE(revolve->enabled(app.context()));
}

TEST_CASE("Move shifts the body and leaves its size alone", "[translate][command]") {
    // Both halves matter. A move that changed the volume would be a scale, and a move that changed
    // nothing would be the no-op this refuses to create.
    Controller app;
    const auto box = aBox(app);
    const auto* before = shapeOf(app, box);
    REQUIRE(before != nullptr);
    const double volumeBefore = before->volume();
    const double xBefore = before->measure().cx;

    const auto* move = commandNamed(app, "feature.translate");
    REQUIRE(move != nullptr);
    REQUIRE(move->enabled(app.context()));
    move->invoke();   // the plain invoke moves 10 mm in X
    app.refresh();

    REQUIRE(app.selection().size() == 1);
    const auto* after = shapeOf(app, app.selection().front());
    REQUIRE(after != nullptr);
    CHECK(after->volume() == Approx(volumeBefore));
    CHECK(after->measure().cx == Approx(xBefore + 10.0).margin(1e-6));
}

TEST_CASE("a move of nothing is refused rather than recorded", "[translate][command]") {
    // Zero is the panel's default, because a move is a vector the user has in mind and guessing one
    // would shift the part the moment the panel opened. Committing that default must not add a
    // feature that changes the part not at all and has to be recognised and deleted.
    Controller app;
    const auto box = aBox(app);
    const std::size_t before = app.tree().size();

    std::string lastStatus;
    app.onStatus([&lastStatus](const std::string& s) { lastStatus = s; });

    REQUIRE(app.beginCommand("feature.translate"));
    REQUIRE(app.commitCommand());   // the parameters are still 0, 0, 0

    INFO("status: " << lastStatus);
    CHECK(app.tree().size() == before);
    CHECK_FALSE(lastStatus.empty());
    (void)box;
}

TEST_CASE("Revolve's default angle is a full turn", "[revolve][command]") {
    // The panel and the compute have to agree: computeRevolve defaults to 2*pi when no angle is
    // stored, so the parameter that appears has to say the same thing rather than a rounder number
    // that quietly changes the result.
    Controller app;
    const auto sketch = aSketch(app);
    REQUIRE(selectAnEdge(app, sketch));
    REQUIRE(app.beginCommand("feature.revolve"));

    const auto& parameters = app.commandParameters();
    REQUIRE(parameters.size() == 1);
    CHECK(parameters.front().name == "angle");
    const auto parsed = cad::units::parseAngle(parameters.front().value);
    REQUIRE(parsed);
    CHECK(parsed.value().base() == Approx(2.0 * std::numbers::pi).margin(1e-6));
    app.cancelCommand();
}

TEST_CASE("the Revolve panel opens with the selection the button accepted", "[revolve][command]") {
    // Same mismatch as Hole: enabled by what is selected, refused by the level. With Auto the
    // default, the angle was never askable and every revolve was a silent full turn.
    Controller app;
    const auto sketch = aSketch(app);

    app.setSelectionLevel(Level::Auto);
    bool picked = false;
    for (std::uint32_t slot = 0; slot < 128 && !picked; ++slot) {
        app.scriptNextPick(slot);
        const auto pick = app.pickAt(10, 10);
        if (!pick.hit || pick.object != sketch) continue;
        app.scriptNextPick(slot);
        if (!app.clickAt(10, 10, /*additive=*/false).changed) continue;
        picked = app.context().selectedEdges == 1;
    }
    REQUIRE(picked);

    const auto* revolve = commandNamed(app, "feature.revolve");
    REQUIRE(revolve != nullptr);
    REQUIRE(revolve->enabled(app.context()));
    REQUIRE(app.beginCommand("feature.revolve"));
    REQUIRE(app.commandParameters().size() == 1);
    CHECK(app.commandParameters().front().name == "angle");
    app.cancelCommand();
}
