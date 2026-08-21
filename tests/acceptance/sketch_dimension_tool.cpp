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
