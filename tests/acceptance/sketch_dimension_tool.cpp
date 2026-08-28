/// Dimensioning geometry that already exists.
///
/// # The gap this closes
///
/// Until now a dimension could only be given to a shape WHILE it was being drawn: type a number
/// before the second click and the segment commits at that size. That is half of dimensioning. The
/// other half — pointing at a line drawn ten minutes ago and saying "40 mm" — had no route at all,
/// and it is the half that makes a sketch parametric, because it is how a model changes shape after
/// the fact.
///
/// The tests below are about the RULES, not the pixels: a dimension is created at the size the
/// geometry currently is, and changing its value moves the geometry. Hit testing needs a camera and
/// is covered where the shell is driven.

#include "cad/app/Controller.h"
#include "cad/sketch/Sketch.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

using namespace cad;
using Catch::Approx;

namespace {

/// Opens a sketch on an existing controller and draws one line in it.
///
/// Takes the controller by reference because `Controller` is non-copyable and non-movable — it owns
/// a GPU backend — so a helper that returns one cannot compile.
sketch::GeoId openSketchWithLine(app::Controller& c) {
    REQUIRE(c.beginSketch() != document::ObjectId{});
    auto* sketch = c.activeSketch();
    REQUIRE(sketch != nullptr);
    return sketch->addLine(0, 0, 100, 0);
}

}   // namespace

TEST_CASE("a dimension is created at the size the geometry already is", "[sketch][dimension]") {
    // Created at the CURRENT value, which is what every CAD application does. At zero it would
    // collapse the sketch the instant it solved — the geometry would obey a number the user never
    // typed.
    app::Controller c;
    const auto line = openSketchWithLine(c);

    const auto index = c.dimensionSketchGeometry(line);
    REQUIRE(index);

    const auto& constraints = c.activeSketch()->constraints();
    REQUIRE(*index < constraints.size());
    CHECK(constraints[*index].kind == sketch::ConstraintKind::Distance);
    CHECK(constraints[*index].value == Approx(100.0));

    // And the geometry did not move: a dimension records what is there.
    const auto* g = c.activeSketch()->find(line);
    REQUIRE(g != nullptr);
    CHECK(std::hypot(g->p[2] - g->p[0], g->p[3] - g->p[1]) == Approx(100.0));
}

TEST_CASE("changing a dimension moves the geometry", "[sketch][dimension]") {
    // The whole point. Without this a dimension is an annotation.
    app::Controller c;
    const auto line = openSketchWithLine(c);
    const auto index = c.dimensionSketchGeometry(line);
    REQUIRE(index);

    REQUIRE(c.setSketchDimension(*index, 40.0));

    const auto* g = c.activeSketch()->find(line);
    REQUIRE(g != nullptr);
    CHECK(std::hypot(g->p[2] - g->p[0], g->p[3] - g->p[1]) == Approx(40.0).margin(1e-6));
}

TEST_CASE("a circle is dimensioned by its radius", "[sketch][dimension]") {
    app::Controller c;
    REQUIRE(c.beginSketch() != document::ObjectId{});
    auto* sketch = c.activeSketch();
    REQUIRE(sketch != nullptr);
    const auto circle = sketch->addCircle(0, 0, 25);

    const auto index = c.dimensionSketchGeometry(circle);
    REQUIRE(index);
    CHECK(sketch->constraints()[*index].kind == sketch::ConstraintKind::Radius);

    REQUIRE(c.setSketchDimension(*index, 10.0));
    const auto* g = sketch->find(circle);
    REQUIRE(g != nullptr);
    CHECK(g->p[2] == Approx(10.0).margin(1e-6));
}

TEST_CASE("nonsense dimensions are refused rather than stored", "[sketch][dimension]") {
    app::Controller c;
    const auto line = openSketchWithLine(c);
    const auto index = c.dimensionSketchGeometry(line);
    REQUIRE(index);

    // A zero or negative length is not a size. Stored, it would either collapse the geometry or
    // make the solver fail on a sketch the user cannot see anything wrong with.
    CHECK_FALSE(c.setSketchDimension(*index, 0.0));
    CHECK_FALSE(c.setSketchDimension(*index, -5.0));
    CHECK(c.activeSketch()->constraints()[*index].value == Approx(100.0));

    // And an index that is not a dimension at all.
    const auto horizontal = c.activeSketch()->horizontal(line);
    CHECK_FALSE(c.setSketchDimension(horizontal, 40.0));
}

TEST_CASE("a point has no dimension", "[sketch][dimension]") {
    app::Controller c;
    REQUIRE(c.beginSketch() != document::ObjectId{});
    auto* sketch = c.activeSketch();
    REQUIRE(sketch != nullptr);
    const auto point = sketch->addPoint(10, 10);

    // Refused with a reason rather than silently doing nothing: "where" and "how big" are different
    // questions, and the second one does not apply.
    CHECK_FALSE(c.dimensionSketchGeometry(point));
}

TEST_CASE("a position lock accepts values a size never would", "[sketch][dimension]") {
    // LockX and LockY are POSITIONS, not sizes, and x = 0 or x = -10 is an ordinary place for a
    // point to be. A single `> 0` guard over every dimension kind refused half the values the sketch
    // can legitimately hold — harmless only for as long as nothing creates a lock through this path,
    // and wrong the moment something does.
    app::Controller c;
    REQUIRE(c.beginSketch() != document::ObjectId{});
    auto* sketch = c.activeSketch();
    REQUIRE(sketch != nullptr);

    const auto point = sketch->addPoint(10, 10);
    const auto lockX = sketch->lockX(point, sketch::PointRef::Start, 10.0);

    CHECK(c.setSketchDimension(lockX, -25.0));
    CHECK(sketch->constraints()[lockX].value == Approx(-25.0));
    CHECK(c.setSketchDimension(lockX, 0.0));
    CHECK(sketch->constraints()[lockX].value == Approx(0.0));

    // And the geometry followed, which is the difference between storing a number and driving one.
    const auto* g = sketch->find(point);
    REQUIRE(g != nullptr);
    CHECK(g->p[0] == Approx(0.0).margin(1e-6));
}

TEST_CASE("a dimension is never set to a non-finite value", "[sketch][dimension]") {
    // Every comparison with NaN is false, so a NaN reaching the solver passes its convergence test
    // and the sketch is reported SOLVED with nonsense in it. This project has had that bug twice.
    app::Controller c;
    const auto line = openSketchWithLine(c);
    const auto index = c.dimensionSketchGeometry(line);
    REQUIRE(index);

    CHECK_FALSE(c.setSketchDimension(*index, std::numeric_limits<double>::quiet_NaN()));
    CHECK_FALSE(c.setSketchDimension(*index, std::numeric_limits<double>::infinity()));
    CHECK(c.activeSketch()->constraints()[*index].value == Approx(100.0));
}

TEST_CASE("an out-of-range constraint index is refused", "[sketch][dimension]") {
    app::Controller c;
    openSketchWithLine(c);
    CHECK_FALSE(c.setSketchDimension(9999, 40.0));
}

#include <cstdio>
TEST_CASE("DIAG preview accumulation", "[diag]") {
    app::Controller c;
    c.setViewportSize(800, 600);
    REQUIRE(c.beginSketch() != document::ObjectId{});
    c.setSketchTool(app::Controller::SketchTool::Line);
    REQUIRE(c.sketchClickAt(400, 300));
    // CONSTANT distance, varying angle: the preview line is the same length every time, so the dash
    // count must not change. Growth here is accumulation, not a longer line.
    for (int i = 0; i < 6; ++i) {
        const double a = i * 0.7;
        c.sketchHoverAt(static_cast<float>(400 + 150 * std::cos(a)),
                        static_cast<float>(300 + 150 * std::sin(a)));
        const auto& f = c.frame();
        std::size_t edgeVerts = 0;
        for (const auto& e : f.edgeBatches) edgeVerts += e.vertexCount;
        std::printf("hover %d: edgeBatches=%zu totalEdgeVerts=%zu\n", i, f.edgeBatches.size(),
                    edgeVerts);
    }
    // And a near one after a far one: the count must SHRINK.
    c.sketchHoverAt(410, 305);
    std::size_t after = 0;
    for (const auto& e : c.frame().edgeBatches) after += e.vertexCount;
    std::printf("after a short hover: totalEdgeVerts=%zu\n", after);
}

// ── the Dimension TOOL, through the shell's path ────────────────────────────────────────
//
// The tests above drive `dimensionSketchGeometry` directly, which is where the rules live. These
// drive the tool: choose it, click a PIXEL, type, press Enter. That path is what was missing — the
// core has been able to do this for days and neither shell could reach it.

namespace {

/// Opens a sketch with one horizontal line and aims the camera at it.
sketch::GeoId sketchWithLine(app::Controller& c) {
    c.setViewportSize(800, 600);
    REQUIRE(c.beginSketch() != document::ObjectId{});
    auto* sketch = c.activeSketch();
    REQUIRE(sketch != nullptr);
    return sketch->addLine(-40, 0, 40, 0);
}

/// Clicks the first pixel whose sketch point lands on the line, and reports whether it worked.
bool clickOnTheLine(app::Controller& c) {
    for (int px = 0; px < 800; px += 4) {
        for (int py = 0; py < 600; py += 4) {
            const auto at = c.sketchPointAt(static_cast<float>(px), static_cast<float>(py));
            if (!at) continue;
            if (std::abs((*at)[0]) > 20.0 || std::abs((*at)[1]) > 0.5) continue;
            if (c.sketchClickAt(static_cast<float>(px), static_cast<float>(py))) return true;
        }
    }
    return false;
}

}   // namespace

TEST_CASE("the Dimension tool places a dimension and takes the keys", "[sketch][dimension][tool]") {
    app::Controller c;
    const auto line = sketchWithLine(c);
    c.setSketchTool(app::Controller::SketchTool::Dimension);

    REQUIRE(clickOnTheLine(c));

    // Created at the size the line already is, and opened for typing in one gesture — requiring a
    // second click to start editing would make the common case two actions.
    REQUIRE(c.editingDimension());
    const auto& constraints = c.activeSketch()->constraints();
    REQUIRE(*c.editingDimension() < constraints.size());
    CHECK(constraints[*c.editingDimension()].kind == sketch::ConstraintKind::Distance);
    CHECK(constraints[*c.editingDimension()].value == Approx(80.0));

    // The readout shows the current value until something is typed, then what is being typed.
    CHECK(c.sketchPreviewText().length.find("80") != std::string::npos);
    REQUIRE(c.typeSketchDimension('4'));
    REQUIRE(c.typeSketchDimension('0'));
    CHECK(c.sketchDimensionInput() == "40");

    REQUIRE(c.commitSketchDimension());
    CHECK_FALSE(c.editingDimension());

    const auto* g = c.activeSketch()->find(line);
    REQUIRE(g != nullptr);
    CHECK(std::hypot(g->p[2] - g->p[0], g->p[3] - g->p[1]) == Approx(40.0).margin(1e-6));
}

TEST_CASE("Escape leaves the dimension it just placed", "[sketch][dimension][tool]") {
    // Escape ends the EDIT, not the dimension. The user asked for a dimension, and removing it
    // would make Escape destructive in a way no other tool is.
    app::Controller c;
    sketchWithLine(c);
    c.setSketchTool(app::Controller::SketchTool::Dimension);
    REQUIRE(clickOnTheLine(c));
    const std::size_t before = c.activeSketch()->constraints().size();

    c.typeSketchDimension('9');
    c.clearSketchDimension();

    CHECK_FALSE(c.editingDimension());
    CHECK(c.activeSketch()->constraints().size() == before);
    CHECK(c.activeSketch()->constraints().back().value == Approx(80.0));   // unchanged
}

TEST_CASE("Enter on an empty field leaves the value alone", "[sketch][dimension][tool]") {
    app::Controller c;
    sketchWithLine(c);
    c.setSketchTool(app::Controller::SketchTool::Dimension);
    REQUIRE(clickOnTheLine(c));

    REQUIRE(c.commitSketchDimension());
    CHECK_FALSE(c.editingDimension());
    CHECK(c.activeSketch()->constraints().back().value == Approx(80.0));
}

TEST_CASE("the Dimension tool draws nothing", "[sketch][dimension][tool]") {
    // The routing. Every tool the drawing state machine does not know used to fall through and
    // quietly draw a line — the bug the circle tool had. A dimension click that hits no curve must
    // leave the sketch exactly as it was.
    app::Controller c;
    sketchWithLine(c);
    const std::size_t before = c.activeSketch()->geometry().size();
    c.setSketchTool(app::Controller::SketchTool::Dimension);

    std::string lastStatus;
    c.onStatus([&lastStatus](const std::string& s) { lastStatus = s; });
    c.sketchClickAt(5, 5);
    c.sketchClickAt(9, 9);

    INFO("status: " << lastStatus);
    CHECK(c.activeSketch()->geometry().size() == before);
    CHECK_FALSE(c.editingDimension());
}

TEST_CASE("dimensions are projected into the viewport", "[sketch][dimension][labels]") {
    // What the shell draws. The labels themselves are separate top-level windows (the only way to
    // paint over the Metal layer), which `--shot` cannot capture — so this is the check that they
    // are being produced at all, and where.
    app::Controller c;
    c.setViewportSize(800, 600);
    REQUIRE(c.beginSketch() != document::ObjectId{});
    auto* sketch = c.activeSketch();
    REQUIRE(sketch != nullptr);

    const auto line = sketch->addLine(-40, -20, 40, -20);
    const auto circle = sketch->addCircle(-10, 10, 14);
    sketch->distance(line, sketch::PointRef::Start, line, sketch::PointRef::End, 80);
    sketch->radius(circle, 14);

    const auto labels = c.sketchDimensionLabels();
    REQUIRE(labels.size() == 2);

    // A radius reads R, a length does not — the drawing convention, and the only way to tell them
    // apart once they are just numbers on screen.
    const auto isRadius = [](const app::Controller::DimensionLabel& l) { return l.radius; };
    const auto radiusLabel = std::find_if(labels.begin(), labels.end(), isRadius);
    REQUIRE(radiusLabel != labels.end());
    CHECK(radiusLabel->text.rfind("R", 0) == 0);
    CHECK(radiusLabel->text.find("14") != std::string::npos);

    const auto lengthLabel = std::find_if(labels.begin(), labels.end(),
                                          [](const auto& l) { return !l.radius; });
    REQUIRE(lengthLabel != labels.end());
    CHECK(lengthLabel->text.find("80") != std::string::npos);

    // Inside the viewport, in device pixels. A label projected outside it would be clamped to an
    // edge by the shell and point at nothing.
    for (const auto& label : labels) {
        INFO("label " << label.text << " at " << label.x << ", " << label.y);
        CHECK(label.x >= 0.0f);
        CHECK(label.x <= 800.0f);
        CHECK(label.y >= 0.0f);
        CHECK(label.y <= 600.0f);
    }

    // The length label sits on the line's midpoint, which is below the circle's centre in sketch
    // space — and the camera looks at the plane with +v up, so it is BELOW on screen too.
    CHECK(lengthLabel->y > radiusLabel->y);
}

TEST_CASE("there are no dimension labels outside a sketch", "[sketch][dimension][labels]") {
    // The shell hides them on this. Left showing, they would float over the model with nothing to
    // attach to.
    app::Controller c;
    c.setViewportSize(800, 600);
    CHECK(c.sketchDimensionLabels().empty());

    REQUIRE(c.beginSketch() != document::ObjectId{});
    auto* sketch = c.activeSketch();
    const auto line = sketch->addLine(0, 0, 50, 0);
    sketch->distance(line, sketch::PointRef::Start, line, sketch::PointRef::End, 50);
    CHECK(c.sketchDimensionLabels().size() == 1);

    c.finishSketch();
    CHECK(c.sketchDimensionLabels().empty());
}
