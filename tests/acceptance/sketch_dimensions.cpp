/// Typing a number while drawing fixes the size, and fixes it for good.
///
/// The distinction this file is really about: a segment that HAPPENS to be 40 mm long and a segment
/// CONSTRAINED to 40 mm look identical the moment you draw them and behave completely differently
/// afterwards. The first forgets the number as soon as anything near it moves. Producing the first
/// while appearing to offer the second is the failure worth guarding against, so every test here
/// checks the constraint, not just the coordinates.

#include "cad/app/Controller.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

using namespace cad;

namespace {

/// A controller already in a sketch with the line tool active. Not returned by value — Controller
/// is not copyable or movable, holding a GPU backend.
void startDrawing(app::Controller& c) {
    c.setViewportSize(1000, 800);
    REQUIRE(c.beginSketch() != document::ObjectId{});
    c.alignCameraToSketch();
    c.setSketchTool(app::Controller::SketchTool::Line);
}

}  // namespace

TEST_CASE("the half-drawn shape reports what it measures", "[sketch][dimension]") {
    app::Controller c;
    startDrawing(c);

    // Nothing pending: nothing to measure. The shell asks on every mouse move, so this has to be
    // an answer rather than a stale number from the last shape.
    CHECK_FALSE(c.sketchPreviewMeasure().valid);

    REQUIRE(c.sketchClickAt(400.0f, 400.0f));
    REQUIRE(c.sketchHoverAt(700.0f, 400.0f));

    const auto measure = c.sketchPreviewMeasure();
    REQUIRE(measure.valid);
    CHECK_FALSE(measure.circle);

    // Against the mapping, not against a number written here: the length is however far apart
    // sketchPointAt says those two pixels are.
    const auto from = c.sketchPointAt(400.0f, 400.0f);
    const auto to = c.sketchPointAt(700.0f, 400.0f);
    REQUIRE(from);
    REQUIRE(to);
    const double dx = (*to)[0] - (*from)[0];
    const double dy = (*to)[1] - (*from)[1];
    CHECK_THAT(measure.length, Catch::Matchers::WithinAbs(std::sqrt(dx * dx + dy * dy), 1e-6));
    CHECK_THAT(measure.angle, Catch::Matchers::WithinAbs(std::atan2(dy, dx) * 180.0 / 3.14159265358979,
                                                         1e-3));
}

TEST_CASE("typing a length commits at that length, with a constraint", "[sketch][dimension]") {
    app::Controller c;
    startDrawing(c);
    const std::size_t before = c.activeSketch()->constraints().size();

    REQUIRE(c.sketchClickAt(400.0f, 400.0f));
    REQUIRE(c.sketchHoverAt(700.0f, 400.0f));   // direction only; the number decides the size

    for (const char ch : std::string("40")) CHECK(c.typeSketchDimension(ch));
    CHECK(c.sketchDimensionInput() == "40");
    REQUIRE(c.commitSketchDimension());

    const auto& geometry = c.activeSketch()->geometry();
    REQUIRE_FALSE(geometry.empty());
    const auto& line = geometry.back();
    REQUIRE(line.kind == sketch::GeoKind::Line);

    // Exactly 40, not "roughly where the pointer was".
    const double dx = line.p[2] - line.p[0];
    const double dy = line.p[3] - line.p[1];
    CHECK_THAT(std::sqrt(dx * dx + dy * dy), Catch::Matchers::WithinAbs(40.0, 1e-6));

    // And DRIVING. A line that is merely 40 long forgets the number the instant the solver moves
    // anything near it; a constrained one does not. This assertion is the whole point of the
    // feature, and it is the one a coordinate check would miss entirely.
    CHECK(c.activeSketch()->constraints().size() > before);
    bool found = false;
    for (const auto& constraint : c.activeSketch()->constraints()) {
        if (constraint.kind == sketch::ConstraintKind::Distance
            && std::abs(constraint.value - 40.0) < 1e-9) {
            found = true;
        }
    }
    CHECK(found);

    // The input is spent, not left to leak into the next shape.
    CHECK(c.sketchDimensionInput().empty());
    CHECK_FALSE(c.sketchPending().has_value());
}

TEST_CASE("a typed radius drives the circle", "[sketch][dimension]") {
    app::Controller c;
    startDrawing(c);
    c.setSketchTool(app::Controller::SketchTool::Circle);

    REQUIRE(c.sketchClickAt(500.0f, 400.0f));
    REQUIRE(c.sketchHoverAt(600.0f, 400.0f));
    CHECK(c.sketchPreviewMeasure().circle);

    for (const char ch : std::string("12.5")) CHECK(c.typeSketchDimension(ch));
    REQUIRE(c.commitSketchDimension());

    const auto& circle = c.activeSketch()->geometry().back();
    REQUIRE(circle.kind == sketch::GeoKind::Circle);
    CHECK_THAT(circle.p[2], Catch::Matchers::WithinAbs(12.5, 1e-6));

    bool found = false;
    for (const auto& constraint : c.activeSketch()->constraints()) {
        if (constraint.kind == sketch::ConstraintKind::Radius
            && std::abs(constraint.value - 12.5) < 1e-9) {
            found = true;
        }
    }
    CHECK(found);
}

TEST_CASE("what the dimension field refuses", "[sketch][dimension]") {
    app::Controller c;
    startDrawing(c);

    // Nothing pending: a keystroke is a shortcut, not a dimension. Swallowing it would make the
    // keyboard feel dead outside of drawing.
    CHECK_FALSE(c.typeSketchDimension('4'));

    REQUIRE(c.sketchClickAt(400.0f, 400.0f));
    REQUIRE(c.sketchHoverAt(700.0f, 400.0f));

    CHECK_FALSE(c.typeSketchDimension('!'));
    CHECK(c.sketchDimensionInput().empty());

    // Text that will not parse leaves the shape pending rather than committing something
    // arbitrary — the user is mid-thought, and losing their line is worse than doing nothing.
    for (const char ch : std::string("xy")) c.typeSketchDimension(ch);
    CHECK_FALSE(c.commitSketchDimension());
    CHECK(c.sketchPending().has_value());

    c.clearSketchDimension();
    CHECK(c.sketchDimensionInput().empty());

    // Zero is refused for the same reason a zero-length click is: it is not a line.
    for (const char ch : std::string("0")) c.typeSketchDimension(ch);
    CHECK_FALSE(c.commitSketchDimension());
    CHECK(c.sketchPending().has_value());
}
