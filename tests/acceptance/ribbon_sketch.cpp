/// The sketch environment's ribbon, as data (app/RibbonLayout): the arrangement AND what each item
/// does, so no shell re-implements "Line sets the tool, Slice toggles, Horizontal applies a
/// constraint" beside the others.
///
/// What these pin is the part that used to live in a shell's lambdas: a tool's button is lit by the
/// model's current tool rather than by the last click, a constraint is offered only when the
/// selection could take it, and every id the data names is one something can run.

#include "cad/app/Controller.h"
#include "cad/app/RibbonLayout.h"

#include <catch2/catch_test_macros.hpp>

#include <set>

using namespace cad;
using app::ribbonItemState;
using app::runRibbonItem;

namespace {

app::Controller& inSketch(app::Controller& c) {
    c.setViewportSize(800, 600);
    REQUIRE(c.beginSketchOn(sketch::Plane::XY) != document::ObjectId{});
    return c;
}

}  // namespace

TEST_CASE("the ribbon follows the environment", "[ribbon][sketch]") {
    app::Controller c;
    c.setViewportSize(800, 600);
    CHECK(&app::ribbonFor(c) == &app::partRibbon());
    inSketch(c);
    CHECK(&app::ribbonFor(c) == &app::sketchRibbon());
    // Inventor collapses the ribbon to one tab while a sketch is open.
    CHECK(app::sketchRibbon().size() == 1);
}

TEST_CASE("a tool's button is lit by the model's tool, not by the last click", "[ribbon][sketch]") {
    app::Controller c;
    inSketch(c);
    REQUIRE(runRibbonItem(c, "sketch.tool.line"));
    CHECK(ribbonItemState(c, "sketch.tool.line").checked);
    CHECK_FALSE(ribbonItemState(c, "sketch.tool.circle").checked);

    // Changed behind the ribbon's back -- by a shortcut, or by another shell -- and the state follows.
    c.setSketchTool(app::Controller::SketchTool::Circle);
    CHECK_FALSE(ribbonItemState(c, "sketch.tool.line").checked);
    CHECK(ribbonItemState(c, "sketch.tool.circle").checked);
}

TEST_CASE("slice is a mode that toggles and reports itself", "[ribbon][sketch]") {
    app::Controller c;
    inSketch(c);
    CHECK_FALSE(ribbonItemState(c, "sketch.slice").checked);
    REQUIRE(runRibbonItem(c, "sketch.slice"));
    CHECK(ribbonItemState(c, "sketch.slice").checked);
    REQUIRE(runRibbonItem(c, "sketch.slice"));
    CHECK_FALSE(ribbonItemState(c, "sketch.slice").checked);
}

TEST_CASE("a constraint is offered only when the selection could take it", "[ribbon][sketch]") {
    app::Controller c;
    inSketch(c);
    // Nothing selected: the model would refuse every one, so none is live.
    for (const char* id : {"sketch.constrain.horizontal", "sketch.constrain.parallel", "sketch.constrain.radius"}) {
        CHECK_FALSE(ribbonItemState(c, id).enabled);
        CHECK_FALSE(runRibbonItem(c, id, 10.0));
    }
}

TEST_CASE("items that ask for a value refuse to run without one", "[ribbon][sketch]") {
    app::Controller c;
    inSketch(c);
    // Offset is enabled in a sketch (it reports its own refusal), and without a distance it must
    // not invent one.
    REQUIRE(ribbonItemState(c, "sketch.offset").enabled);
    CHECK_FALSE(runRibbonItem(c, "sketch.offset"));
}

TEST_CASE("every sketch ribbon id is one something can run", "[ribbon][sketch]") {
    app::Controller c;
    inSketch(c);
    std::set<std::string> ids, keys;
    for (const auto& tab : app::sketchRibbon()) {
        for (const auto& section : tab.sections) {
            for (const auto& item : section.items) {
                REQUIRE_FALSE(item.id.empty());
                CHECK(ids.insert(item.id).second);  // no id twice
                if (!item.shortcut.empty()) CHECK(keys.insert(item.shortcut).second);  // no key twice
                const bool constraint = item.id.rfind("sketch.constrain.", 0) == 0;
                // Constraints wait for a selection; everything else is live the moment a sketch is
                // open. An id nothing resolves answers disabled here, so a typo fails this.
                if (!constraint) {
                    INFO(item.id);
                    CHECK(ribbonItemState(c, item.id).enabled);
                }
            }
        }
    }
    // Finish and Cancel are Controller commands, resolved through commands() like the model tab's.
    CHECK(ids.count("sketch.finish") == 1);
    CHECK(ids.count("sketch.cancel") == 1);
}

TEST_CASE("sketch items are dead outside a sketch", "[ribbon][sketch]") {
    app::Controller c;
    c.setViewportSize(800, 600);
    for (const char* id : {"sketch.tool.line", "sketch.slice", "sketch.lookat", "sketch.delete"}) {
        INFO(id);
        CHECK_FALSE(ribbonItemState(c, id).enabled);
        CHECK_FALSE(runRibbonItem(c, id));
    }
}
