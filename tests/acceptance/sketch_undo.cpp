/// Undo inside a sketch.
///
/// # What was wrong
///
/// The document's history records FEATURES, and a sketch is one feature however long it took to
/// draw. So every edit inside the sketch environment was outside undo entirely: a twenty-minute
/// sketch had exactly one undo step, "Edit Sketch". Worse, pressing undo mid-sketch reverted the
/// document while the working copy carried on unchanged, leaving the two describing different
/// models.
///
/// # What these tests actually guard
///
/// Not "undo works" — that is one test. The risk is a mutation nobody snapshotted: the sketch gains
/// operations regularly, and each new one is a chance to forget. So the cases below walk EVERY
/// mutating operation and assert that each is undoable, which is the thing that rots.

#include "cad/app/Controller.h"
#include "cad/sketch/Sketch.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <string>

using namespace cad;
using Catch::Approx;

namespace {

/// A sketch with one horizontal line, ready to be edited.
sketch::GeoId lineIn(app::Controller& c) {
    REQUIRE(c.beginSketchOn(sketch::Plane::XY) != document::ObjectId{});
    auto* sketch = c.activeSketch();
    REQUIRE(sketch != nullptr);
    return sketch->addLine(0, 0, 100, 0);
}

}   // namespace

TEST_CASE("every sketch edit can be undone", "[sketch][undo]") {
    // The table is the point. Adding an operation to the sketch and not to this list is the mistake
    // this test exists to catch, and a reviewer who sees an operation missing here has been told
    // exactly what to check.
    struct Case {
        const char* what;
        std::function<void(app::Controller&, sketch::GeoId)> edit;
    };
    const std::vector<Case> cases{
        {"draw a line",
         [](app::Controller& c, sketch::GeoId) {
             c.setSketchTool(app::Controller::SketchTool::Line);
             c.sketchClickAt(400, 300);
             c.sketchClickAt(500, 380);
         }},
        {"delete geometry",
         [](app::Controller& c, sketch::GeoId line) {
             c.selectSketchGeometry(line, false);
             c.deleteSketchSelection();
         }},
        {"dimension a curve",
         [](app::Controller& c, sketch::GeoId line) { c.dimensionSketchGeometry(line); }},
        {"apply a constraint",
         [](app::Controller& c, sketch::GeoId line) {
             c.selectSketchGeometry(line, false);
             c.applySketchConstraint(sketch::ConstraintKind::Horizontal);
         }},
        {"offset a curve",
         [](app::Controller& c, sketch::GeoId line) {
             c.selectSketchGeometry(line, false);
             c.offsetSketchSelection(5.0);
         }},
        {"clear the sketch",
         [](app::Controller& c, sketch::GeoId) { c.clearSketch(); }},
    };

    for (const auto& item : cases) {
        app::Controller c;
        const auto line = lineIn(c);
        const auto* sketch = c.activeSketch();
        REQUIRE(sketch != nullptr);

        const std::string before = sketch->serialize();
        item.edit(c, line);

        INFO("operation: " << item.what);
        const std::string after = c.activeSketch()->serialize();
        // The edit has to have DONE something, or the undo assertion below would pass for a reason
        // that has nothing to do with undo.
        REQUIRE(after != before);

        REQUIRE(c.canUndoSketch());
        REQUIRE(c.undoSketch());
        CHECK(c.activeSketch()->serialize() == before);
    }
}

TEST_CASE("undo inside a sketch does not touch the document", "[sketch][undo]") {
    // The failure this replaces. Undoing the document while a sketch is open reverts features
    // underneath a working copy that carries on unchanged — after which the two describe different
    // models and nothing on screen says so.
    app::Controller c;
    REQUIRE(c.beginCommand("feature.box"));
    REQUIRE(c.commitCommand());

    const auto line = lineIn(c);
    // Counted AFTER the sketch exists: opening one legitimately adds a feature, so a baseline taken
    // before it would fail for a reason that has nothing to do with undo.
    const std::size_t objects = c.document().size();
    c.selectSketchGeometry(line, false);
    c.offsetSketchSelection(5.0);

    REQUIRE(c.undo());                       // routed to the sketch
    CHECK(c.document().size() == objects);   // the box is untouched
    CHECK(c.environment() == app::Environment::Sketch);
}

TEST_CASE("redo replays what undo took back", "[sketch][undo]") {
    app::Controller c;
    const auto line = lineIn(c);
    c.selectSketchGeometry(line, false);
    c.offsetSketchSelection(5.0);
    const std::string edited = c.activeSketch()->serialize();

    REQUIRE(c.undoSketch());
    REQUIRE(c.canRedoSketch());
    REQUIRE(c.redoSketch());
    CHECK(c.activeSketch()->serialize() == edited);
}

TEST_CASE("a new edit ends the redo branch", "[sketch][undo]") {
    // As in every editor: the future you could have gone back to is not the future you are in.
    app::Controller c;
    const auto line = lineIn(c);
    c.selectSketchGeometry(line, false);
    c.offsetSketchSelection(5.0);
    REQUIRE(c.undoSketch());
    REQUIRE(c.canRedoSketch());

    c.selectSketchGeometry(line, false);
    c.offsetSketchSelection(9.0);
    CHECK_FALSE(c.canRedoSketch());
}

TEST_CASE("the sketch history belongs to the editing session", "[sketch][undo]") {
    // Re-opening a sketch must not offer undo steps from last time: they describe a sketch the
    // document has since committed, and applying one would undo past what was already recorded.
    app::Controller c;
    const auto line = lineIn(c);
    c.selectSketchGeometry(line, false);
    c.offsetSketchSelection(5.0);
    REQUIRE(c.canUndoSketch());

    const auto id = c.document().ids().back();
    c.finishSketch();
    c.editSketch(id);
    CHECK_FALSE(c.canUndoSketch());
}

TEST_CASE("a click that draws nothing does not consume an undo step", "[sketch][undo]") {
    // The first click of a line starts a chain and changes no geometry. An undo step that restores
    // an identical sketch reads as undo being broken: the user presses it and sees nothing happen.
    app::Controller c;
    lineIn(c);
    c.setSketchTool(app::Controller::SketchTool::Line);
    c.sketchClickAt(400, 300);
    CHECK_FALSE(c.canUndoSketch());
}
