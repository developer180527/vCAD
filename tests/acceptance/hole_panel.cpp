// Drilling a hole the way the IPAD does it: through the command panel.
//
// hole_command.cpp drives `invoke()`, which is the desktop's path and the one that was proven. The
// iPad never takes it -- its rail calls `beginCommand`, shows a panel of typed values, and then
// `commitCommand`. Those are two different routes into the same feature, and a hole reported as
// failing on the iPad while every desktop test passed is exactly the shape of a gap between them.

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

double volumeOf(const Controller& app, cad::document::ObjectId id) {
    const auto object = app.document().find(id);
    if (!object || object->output() == nullptr) return -1.0;
    return object->output()->shape.volume();
}

/// The error a failed feature is showing in the browser, or "" if it computed.
std::string errorOf(const Controller& app, cad::document::ObjectId id) {
    for (const auto& item : app.tree()) {
        if (item.id == id) return item.error;
    }
    return "<no such row>";
}

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

/// A box built through the PANEL, as the iPad builds one.
cad::document::ObjectId aPanelBox(Controller& app) {
    REQUIRE(app.beginCommand("feature.box"));
    REQUIRE(app.commitCommand());
    app.refresh();
    REQUIRE(app.selection().size() == 1);
    return app.selection().front();
}

}  // namespace

TEST_CASE("a hole committed from the panel removes material", "[hole][panel]") {
    Controller app;
    const auto box = aPanelBox(app);
    const double before = volumeOf(app, box);
    REQUIRE(before > 0.0);

    REQUIRE(selectAFace(app, box));
    REQUIRE(app.beginCommand("feature.hole"));
    REQUIRE(app.commitCommand());
    app.refresh();

    REQUIRE(app.selection().size() == 1);
    const auto hole = app.selection().front();

    // The error FIRST, and as the message rather than as a bool. A failed feature is what the user
    // reported seeing in the browser, and an assertion that only says "volume was -1" makes the
    // reader go and reproduce it by hand to learn why.
    CHECK(errorOf(app, hole) == "");

    const double after = volumeOf(app, hole);
    REQUIRE(after > 0.0);
    const double expected = std::numbers::pi * 4.0 * 4.0 * 10.0;
    CHECK(after == Approx(before - expected).epsilon(0.02));
}

TEST_CASE("the panel seeds a hole's diameter and depth", "[hole][panel]") {
    // `commitCommand` reads the typed values through a helper that returns 0.0 when it cannot find
    // or parse one, and a zero-diameter cylinder is a failed feature rather than a refused command.
    // So the defaults being present and parseable IS the precondition for the case above.
    Controller app;
    const auto box = aPanelBox(app);
    REQUIRE(selectAFace(app, box));
    REQUIRE(app.beginCommand("feature.hole"));

    bool sawDiameter = false;
    bool sawDepth = false;
    for (const auto& p : app.commandParameters()) {
        if (p.name == "diameter") sawDiameter = !p.value.empty();
        if (p.name == "depth") sawDepth = !p.value.empty();
    }
    CHECK(sawDiameter);
    CHECK(sawDepth);
}

// NOT TESTED HERE: whether a finger-sized tap at SelectionLevel::Auto -- the iPad's actual picking
// -- lands on a face or on an edge near it. That is the one remaining difference between the route
// these tests take and the route a user takes, and it is deliberately out of reach: the null
// backend has no rasteriser (see `scriptNextPick`), so a headless test can script WHICH slot was
// hit but cannot ask what a real aperture would have ranked first. Answering it needs the device.
