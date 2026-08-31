/// Drawing, as a person performs it. Four clicks make a rectangle.
///
/// # Why this file exists
///
/// Every other sketch test asserts model facts — one line was added, it ends where sketchPointAt
/// says, a constraint appeared. All of those passed while the Line tool took TWO CLICKS PER SEGMENT
/// and threw the endpoint away, which is a tool nobody can draw a closed shape with. The assertions
/// were true and the feature was unusable.
///
/// So these tests are written as a click SEQUENCE and assert the outcome a user cares about: is
/// there a closed profile. That is the question a count of lines cannot answer.

#include "cad/app/Controller.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using namespace cad;

namespace {

void startSketch(app::Controller& c) {
    c.setViewportSize(1000, 800);
    REQUIRE(c.beginSketchOn(cad::sketch::Plane::XY) != document::ObjectId{});
    c.alignCameraToSketch();
    c.setSketchTool(app::Controller::SketchTool::Line);
    // The seeded rectangle would make "is there a closed profile" true before a single click.
    c.clearSketch();
}

}  // namespace

TEST_CASE("four clicks and a close make a rectangle", "[sketch][drawing]") {
    app::Controller c;
    startSketch(c);

    // Exactly what a person does: corner, corner, corner, corner, back to the first.
    REQUIRE(c.sketchClickAt(300.0f, 300.0f));
    REQUIRE(c.sketchClickAt(700.0f, 300.0f));
    REQUIRE(c.sketchClickAt(700.0f, 550.0f));
    REQUIRE(c.sketchClickAt(300.0f, 550.0f));
    REQUIRE(c.sketchClickAt(300.0f, 300.0f));   // onto the start point, closing the loop

    // FOUR segments from five clicks. Two-clicks-per-segment would give two, and the shape would
    // not close — which is exactly the bug this file was written to catch.
    CHECK(c.activeSketch()->geometry().size() == 4);

    // The assertion that matters: it bounds an area. toFace() is what Extrude needs, and it is the
    // only check that can tell a rectangle from four lines that nearly meet.
    CHECK(bool(c.activeSketch()->toFace()));

    // Closing the loop also ends the chain — otherwise the next click starts a fifth segment from
    // a corner the user considers finished.
    CHECK_FALSE(c.sketchPending().has_value());
}

TEST_CASE("the chain continues from the last endpoint", "[sketch][drawing]") {
    app::Controller c;
    startSketch(c);

    REQUIRE(c.sketchClickAt(300.0f, 300.0f));
    REQUIRE(c.sketchClickAt(500.0f, 300.0f));
    CHECK(c.activeSketch()->geometry().size() == 1);
    // Still drawing: the endpoint became the next segment's start.
    REQUIRE(c.sketchPending().has_value());

    REQUIRE(c.sketchClickAt(500.0f, 450.0f));
    REQUIRE(c.activeSketch()->geometry().size() == 2);

    // The second segment STARTS where the first ended. Without this the two are unrelated lines
    // that happen to be near each other, and no amount of careful clicking will close a profile.
    const auto& first = c.activeSketch()->geometry()[0];
    const auto& second = c.activeSketch()->geometry()[1];
    CHECK_THAT(second.p[0], Catch::Matchers::WithinAbs(first.p[2], 1e-9));
    CHECK_THAT(second.p[1], Catch::Matchers::WithinAbs(first.p[3], 1e-9));
}

TEST_CASE("Escape ends the chain without ending the tool", "[sketch][drawing]") {
    app::Controller c;
    startSketch(c);

    REQUIRE(c.sketchClickAt(300.0f, 300.0f));
    REQUIRE(c.sketchClickAt(500.0f, 300.0f));
    REQUIRE(c.sketchPending().has_value());

    c.endSketchChain();
    CHECK_FALSE(c.sketchPending().has_value());
    CHECK(c.activeSketch()->geometry().size() == 1);   // nothing added, nothing removed

    // The TOOL is still Line: Escape ends the run, it does not put the user back in Select. The
    // next click starts a fresh chain rather than doing nothing.
    CHECK(c.sketchTool() == app::Controller::SketchTool::Line);
    REQUIRE(c.sketchClickAt(600.0f, 600.0f));
    CHECK(c.sketchPending().has_value());
    CHECK(c.activeSketch()->geometry().size() == 1);
}

TEST_CASE("a click near an existing endpoint joins it exactly", "[sketch][drawing]") {
    app::Controller c;
    startSketch(c);

    REQUIRE(c.sketchClickAt(300.0f, 300.0f));
    REQUIRE(c.sketchClickAt(700.0f, 300.0f));
    c.endSketchChain();

    // A new chain starting NEAR the first line's end — off by a few pixels, as a hand is.
    REQUIRE(c.sketchClickAt(703.0f, 302.0f));
    REQUIRE(c.sketchClickAt(700.0f, 550.0f));

    const auto& first = c.activeSketch()->geometry()[0];
    const auto& second = c.activeSketch()->geometry()[1];

    // EXACTLY equal, not merely close. "Near enough" is what leaves a profile open by a hundredth
    // of a millimetre — invisible on screen and fatal to the extrude.
    CHECK_THAT(second.p[0], Catch::Matchers::WithinAbs(first.p[2], 1e-9));
    CHECK_THAT(second.p[1], Catch::Matchers::WithinAbs(first.p[3], 1e-9));
}

TEST_CASE("a roughly horizontal line is made horizontal", "[sketch][drawing]") {
    app::Controller c;
    startSketch(c);

    // Two pixels of droop over four hundred — a hand aiming at horizontal.
    REQUIRE(c.sketchClickAt(300.0f, 300.0f));
    REQUIRE(c.sketchClickAt(700.0f, 302.0f));

    const auto& line = c.activeSketch()->geometry().back();
    // Inference is why a Fusion sketch ends up nearly fully constrained without a dimensioning
    // pass. Without it every hand-drawn line stays free, and the sketch is a drawing.
    CHECK_THAT(line.p[3], Catch::Matchers::WithinAbs(line.p[1], 1e-9));

    bool horizontal = false;
    for (const auto& constraint : c.activeSketch()->constraints()) {
        if (constraint.kind == sketch::ConstraintKind::Horizontal) horizontal = true;
    }
    CHECK(horizontal);
}

TEST_CASE("Tab locks the length so the mouse only aims", "[sketch][drawing][lock]") {
    app::Controller c;
    startSketch(c);

    REQUIRE(c.sketchClickAt(300.0f, 300.0f));
    REQUIRE(c.sketchHoverAt(700.0f, 300.0f));

    for (const char ch : std::string("40")) REQUIRE(c.typeSketchDimension(ch));
    REQUIRE(c.lockSketchDimension());

    // Locked, and the typed text is spent — it has become a value, not a half-entered string.
    REQUIRE(c.sketchLockedLength().has_value());
    CHECK_THAT(*c.sketchLockedLength(), Catch::Matchers::WithinAbs(40.0, 1e-9));
    CHECK(c.sketchDimensionInput().empty());

    // The whole point of locking: moving the pointer changes the DIRECTION and not the length.
    // Without it the value the user just decided tracks the mouse and is immediately lost.
    REQUIRE(c.sketchHoverAt(500.0f, 600.0f));
    const auto measure = c.sketchPreviewMeasure();
    REQUIRE(measure.valid);
    CHECK_THAT(measure.length, Catch::Matchers::WithinAbs(40.0, 1e-6));

    // Clicking commits at the locked length, in the direction the pointer was aiming.
    REQUIRE(c.sketchClickAt(500.0f, 600.0f));
    const auto& line = c.activeSketch()->geometry().back();
    const double dx = line.p[2] - line.p[0];
    const double dy = line.p[3] - line.p[1];
    CHECK_THAT(std::sqrt(dx * dx + dy * dy), Catch::Matchers::WithinAbs(40.0, 1e-6));

    // Driving, like a typed dimension — a locked length that the solver can undo was never locked.
    bool driven = false;
    for (const auto& constraint : c.activeSketch()->constraints()) {
        if (constraint.kind == sketch::ConstraintKind::Distance
            && std::abs(constraint.value - 40.0) < 1e-9) {
            driven = true;
        }
    }
    CHECK(driven);

    // Spent on commit. A lock that survived into the next segment would silently force every
    // following line to 40 mm, which is the opposite of helpful.
    CHECK_FALSE(c.sketchLockedLength().has_value());
}

TEST_CASE("what Tab refuses to lock", "[sketch][drawing][lock]") {
    app::Controller c;
    startSketch(c);

    // Nothing typed and nothing pending: Tab is a shell shortcut, not a dimension lock.
    CHECK_FALSE(c.lockSketchDimension());

    REQUIRE(c.sketchClickAt(300.0f, 300.0f));
    REQUIRE(c.sketchHoverAt(700.0f, 300.0f));

    // Nothing typed, but a shape is pending: locking the CURRENT measured length is a reasonable
    // reading, and it is the one Fusion offers — Tab with an empty box locks what is shown.
    REQUIRE(c.lockSketchDimension());
    REQUIRE(c.sketchLockedLength().has_value());

    c.endSketchChain();
    // Escape clears the lock with the chain. A lock outliving the run it belonged to would apply
    // to a segment the user never sized.
    CHECK_FALSE(c.sketchLockedLength().has_value());

    REQUIRE(c.sketchClickAt(300.0f, 300.0f));
    REQUIRE(c.sketchHoverAt(700.0f, 300.0f));
    for (const char ch : std::string("xx")) c.typeSketchDimension(ch);
    // Unparseable text locks nothing and leaves the text alone, so the user can correct it.
    CHECK_FALSE(c.lockSketchDimension());
    CHECK_FALSE(c.sketchLockedLength().has_value());
    CHECK(c.sketchDimensionInput() == "xx");
}

TEST_CASE("a double click ends the chain", "[sketch][drawing]") {
    app::Controller c;
    startSketch(c);

    REQUIRE(c.sketchClickAt(300.0f, 300.0f));
    REQUIRE(c.sketchClickAt(500.0f, 300.0f));
    REQUIRE(c.sketchPending().has_value());

    // Qt delivers press, release, then a DOUBLE-CLICK event in place of the second press — so the
    // second click never reaches sketchClickAt and the chain cannot end by itself. This is the
    // event the shell forwards.
    c.endSketchChain();
    CHECK_FALSE(c.sketchPending().has_value());
    CHECK(c.activeSketch()->geometry().size() == 1);
    CHECK(c.sketchTool() == app::Controller::SketchTool::Line);
}
