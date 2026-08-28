// Trimming a sketch curve back to its neighbours.
//
// The most-reached-for sketch edit, and the reason its absence forces every profile to be drawn
// exactly right the first time. All of it is arithmetic over a `Sketch`, so all of it is testable
// without a screen — which matters, because trim is the kind of geometry code that reads correctly
// and is wrong on the third case.
//
// The cases below are the ones that decide whether the tool is usable: an overhang past a corner, a
// span in the middle, a chain segment whose cuts sit at its very ends, a circle becoming an arc,
// and what happens to the constraints that were attached to what moved.

#include "cad/sketch/Sketch.h"
#include "cad/sketch/Trim.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <numbers>

using namespace cad::sketch;
using Catch::Approx;

namespace {

const Geometry& geometryOf(const Sketch& sketch, GeoId id) {
    const Geometry* g = sketch.find(id);
    REQUIRE(g != nullptr);
    return *g;
}

double lengthOf(const Geometry& g) {
    return std::hypot(g.p[2] - g.p[0], g.p[3] - g.p[1]);
}

}  // namespace

TEST_CASE("an overhang past a corner is cut back to it", "[trim][sketch]") {
    // The ordinary case: two lines crossing in a T, and the bit sticking out gets clicked away.
    Sketch sketch;
    const auto horizontal = sketch.addLine(0, 0, 100, 0);
    sketch.addLine(60, -50, 60, 50);   // crosses at x = 60

    const auto result = trim(sketch, horizontal, {80, 0});   // click the overhang
    REQUIRE(result);
    CHECK_FALSE(result.value().removedWhole);
    CHECK_FALSE(result.value().splitInto);

    // Kept its id — which is what keeps its constraints — and now ends at the crossing.
    const Geometry& g = geometryOf(sketch, horizontal);
    CHECK(g.p[0] == Approx(0.0));
    CHECK(g.p[2] == Approx(60.0));
    CHECK(sketch.geometry().size() == 2);
}

TEST_CASE("clicking the other side trims the other end", "[trim][sketch]") {
    // Same geometry, different click. If the click did not decide which span goes, the tool would
    // be a coin toss on every crossing curve.
    Sketch sketch;
    const auto horizontal = sketch.addLine(0, 0, 100, 0);
    sketch.addLine(60, -50, 60, 50);

    REQUIRE(trim(sketch, horizontal, {20, 0}));
    const Geometry& g = geometryOf(sketch, horizontal);
    CHECK(g.p[0] == Approx(60.0));
    CHECK(g.p[2] == Approx(100.0));
}

TEST_CASE("a span between two crossings splits the curve in two", "[trim][sketch]") {
    Sketch sketch;
    const auto horizontal = sketch.addLine(0, 0, 100, 0);
    sketch.addLine(30, -50, 30, 50);
    sketch.addLine(70, -50, 70, 50);

    const auto result = trim(sketch, horizontal, {50, 0});   // the middle span
    REQUIRE(result);
    REQUIRE(result.value().splitInto);

    // The original id keeps the FIRST piece, so anything constrained to it stays attached.
    const Geometry& first = geometryOf(sketch, horizontal);
    CHECK(first.p[0] == Approx(0.0));
    CHECK(first.p[2] == Approx(30.0));

    const Geometry& second = geometryOf(sketch, *result.value().splitInto);
    CHECK(second.p[0] == Approx(70.0));
    CHECK(second.p[2] == Approx(100.0));
    CHECK(sketch.geometry().size() == 4);   // two verticals plus the two halves
}

TEST_CASE("a curve nothing crosses is removed entirely", "[trim][sketch]") {
    // There is nothing to cut back TO. Refusing would make the tool useless in exactly the place a
    // stray line most needs deleting, so trim doubles as delete for an uncrossed curve.
    Sketch sketch;
    const auto lonely = sketch.addLine(0, 0, 10, 10);

    const auto result = trim(sketch, lonely, {5, 5});
    REQUIRE(result);
    CHECK(result.value().removedWhole);
    CHECK(sketch.geometry().empty());
    CHECK(sketch.find(lonely) == nullptr);
}

TEST_CASE("a chain segment joined at both ends is removed whole", "[trim][sketch]") {
    // The case a naive implementation gets wrong. Every side of a closed rectangle meets its
    // neighbours AT ITS ENDPOINTS, so the cuts are at t=0 and t=1 and the span between them is the
    // entire line. Splitting there would leave two zero-length ghosts the user can neither see nor
    // select.
    Sketch sketch;
    const auto bottom = sketch.addLine(0, 0, 40, 0);
    sketch.addLine(40, 0, 40, 25);
    sketch.addLine(40, 25, 0, 25);
    sketch.addLine(0, 25, 0, 0);

    const auto result = trim(sketch, bottom, {20, 0});
    REQUIRE(result);
    CHECK(result.value().removedWhole);
    CHECK(sketch.geometry().size() == 3);
}

TEST_CASE("a circle cut twice becomes an arc", "[trim][sketch]") {
    // A circle is a ring: it has no ends, so it needs two cuts before any span has both of them.
    Sketch sketch;
    const auto circle = sketch.addCircle(0, 0, 50);
    sketch.addLine(-100, 0, 100, 0);   // cuts at angle 0 and angle pi

    // Click the TOP half.
    const auto result = trim(sketch, circle, {0, 50});
    REQUIRE(result);
    CHECK_FALSE(result.value().removedWhole);

    const Geometry& g = geometryOf(sketch, circle);
    REQUIRE(g.kind == GeoKind::Arc);
    CHECK(g.p[2] == Approx(50.0));   // radius unchanged

    // The BOTTOM half survives: an arc from pi round to 2pi (that is, back to 0).
    const double start = g.p[3];
    const double end = g.p[4];
    CHECK(start == Approx(std::numbers::pi).margin(1e-6));
    CHECK(std::fmod(end + 2.0 * std::numbers::pi, 2.0 * std::numbers::pi)
          == Approx(0.0).margin(1e-6));
}

TEST_CASE("a circle cut once is removed rather than half-trimmed", "[trim][sketch]") {
    Sketch sketch;
    const auto circle = sketch.addCircle(0, 0, 50);
    sketch.addLine(50, 0, 100, 0);   // touches the rim at exactly one point

    const auto result = trim(sketch, circle, {0, 50});
    REQUIRE(result);
    CHECK(result.value().removedWhole);
    CHECK(sketch.find(circle) == nullptr);
}

TEST_CASE("trim drops the constraints that pinned what moved", "[trim][sketch]") {
    // A trim moves endpoints by definition. A coincidence or a length pinning one of them would be
    // dragged back by the next solve, or report the sketch over-constrained — and both read as the
    // trim having failed rather than as a constraint having been kept.
    Sketch sketch;
    const auto horizontal = sketch.addLine(0, 0, 100, 0);
    const auto vertical = sketch.addLine(60, -50, 60, 50);

    sketch.horizontal(horizontal);                                               // shape: survives
    sketch.distance(horizontal, PointRef::Start, horizontal, PointRef::End, 100); // length: dropped
    sketch.coincident(horizontal, PointRef::End, vertical, PointRef::End);       // a point: dropped
    REQUIRE(sketch.constraints().size() == 3);

    const auto result = trim(sketch, horizontal, {80, 0});
    REQUIRE(result);
    CHECK(result.value().constraintsDropped == 2);

    REQUIRE(sketch.constraints().size() == 1);
    CHECK(sketch.constraints().front().kind == ConstraintKind::Horizontal);

    // And the survivor still solves — the point of dropping them.
    const auto report = sketch.solve();
    INFO(report.message);
    CHECK(report.solved);
}

TEST_CASE("trimming refuses what it cannot do, with a reason", "[trim][sketch]") {
    Sketch sketch;
    const auto point = sketch.addPoint(5, 5);
    const auto line = sketch.addLine(0, 0, 10, 0);

    // A point has no span.
    const auto onPoint = trim(sketch, point, {5, 5});
    REQUIRE_FALSE(onPoint);
    CHECK_FALSE(onPoint.error().message.empty());

    // A click nowhere near the curve cannot say which span was meant, and guessing would delete
    // something at random.
    const auto miss = trim(sketch, line, {500, 500});
    REQUIRE_FALSE(miss);
    CHECK_FALSE(miss.error().message.empty());

    // An id from another sketch.
    const auto unknown = trim(sketch, 9999, {0, 0});
    REQUIRE_FALSE(unknown);
    CHECK(sketch.geometry().size() == 2);   // nothing was touched by any of the three
}

TEST_CASE("intersections are only where curves actually cross", "[trim][sketch]") {
    // The filtering that makes trim mean anything: two curves whose infinite extensions would meet
    // do not intersect, or every stray line in the sketch would cut every other one.
    Sketch sketch;
    const auto a = sketch.addLine(0, 0, 10, 0);
    const auto b = sketch.addLine(50, -10, 50, 10);   // crosses the EXTENSION, not the segment
    CHECK(intersections(geometryOf(sketch, a), geometryOf(sketch, b)).empty());

    const auto c = sketch.addLine(5, -10, 5, 10);     // genuinely crosses
    const auto hits = intersections(geometryOf(sketch, a), geometryOf(sketch, c));
    REQUIRE(hits.size() == 1);
    CHECK(hits.front()[0] == Approx(5.0));
    CHECK(hits.front()[1] == Approx(0.0));

    // A line through a circle crosses it twice; a line that misses it does not touch it.
    const auto circle = sketch.addCircle(0, 0, 20);
    const auto through = sketch.addLine(-30, 0, 30, 0);
    CHECK(intersections(geometryOf(sketch, circle), geometryOf(sketch, through)).size() == 2);
    const auto past = sketch.addLine(-30, 25, 30, 25);
    CHECK(intersections(geometryOf(sketch, circle), geometryOf(sketch, past)).empty());
}

TEST_CASE("trimming an arc keeps it an arc", "[trim][sketch]") {
    Sketch sketch;
    // A half circle above the x axis, from 0 to pi.
    const auto arc = sketch.addArc(0, 0, 50, 0.0, std::numbers::pi);
    sketch.addLine(0, 0, 0, 100);   // cuts it at the top, angle pi/2

    // Click the left quarter, between pi/2 and pi.
    const auto result = trim(sketch, arc, {-35.0, 35.0});
    REQUIRE(result);
    CHECK_FALSE(result.value().removedWhole);

    const Geometry& g = geometryOf(sketch, arc);
    CHECK(g.kind == GeoKind::Arc);
    CHECK(g.p[3] == Approx(0.0).margin(1e-6));
    CHECK(g.p[4] == Approx(std::numbers::pi / 2.0).margin(1e-6));
}

// ── through the Controller, in pixels ───────────────────────────────────────────────────
//
// The tests above drive `sketch::trim` in sketch coordinates, which is where the algorithm lives.
// These drive it the way the shell does: set the tool, click at a PIXEL, and let the camera decide
// what that means. That is the path that was missing, and it exercises three things the pure maths
// cannot -- the tool routing, the hit testing, and the unprojection.

#include "cad/app/Controller.h"

TEST_CASE("the Trim tool cuts the curve under the pointer", "[trim][sketch][controller]") {
    cad::app::Controller app;
    app.setViewportSize(800, 600);
    REQUIRE(app.beginSketch() != cad::document::ObjectId{});

    auto* sketch = app.activeSketch();
    REQUIRE(sketch != nullptr);
    const auto horizontal = sketch->addLine(-40, 0, 40, 0);
    sketch->addLine(10, -30, 10, 30);   // crosses at x = 10

    // The camera is aimed at the sketch plane on entry, so the sketch's own coordinates map to
    // pixels through it. Find the pixel for a point on the overhang by asking the same maths the
    // click will use, rather than assuming where the centre of the viewport lands.
    app.setSketchTool(cad::app::Controller::SketchTool::Trim);
    CHECK(app.sketchTool() == cad::app::Controller::SketchTool::Trim);

    bool trimmed = false;
    for (int px = 0; px < 800 && !trimmed; px += 4) {
        for (int py = 0; py < 600 && !trimmed; py += 4) {
            const auto at = app.sketchPointAt(static_cast<float>(px), static_cast<float>(py));
            if (!at) continue;
            // A point clearly on the overhang: past the crossing, on the line.
            if ((*at)[0] < 25.0 || (*at)[0] > 35.0 || std::abs((*at)[1]) > 0.5) continue;
            trimmed = app.sketchClickAt(static_cast<float>(px), static_cast<float>(py));
        }
    }
    REQUIRE(trimmed);

    // Cut back to the crossing, and still two curves — the trim shortened rather than deleted.
    const Geometry* g = app.activeSketch()->find(horizontal);
    REQUIRE(g != nullptr);
    CHECK(g->p[0] == Approx(-40.0).margin(1e-3));
    CHECK(g->p[2] == Approx(10.0).margin(1e-3));
    CHECK(app.activeSketch()->geometry().size() == 2);
}

TEST_CASE("Trim consumes the click rather than drawing with it", "[trim][sketch][controller]") {
    // The routing. Before Trim existed every tool fell through to the drawing state machine, so a
    // tool it did not know would quietly draw a line — the same bug the circle tool had, one tool
    // along. A click that hits nothing must add nothing.
    cad::app::Controller app;
    app.setViewportSize(800, 600);
    REQUIRE(app.beginSketch() != cad::document::ObjectId{});
    app.setSketchTool(cad::app::Controller::SketchTool::Trim);

    std::string lastStatus;
    app.onStatus([&lastStatus](const std::string& s) { lastStatus = s; });

    app.sketchClickAt(400, 300);
    app.sketchClickAt(420, 320);

    INFO("status: " << lastStatus);
    CHECK(app.activeSketch()->geometry().empty());
    CHECK_FALSE(lastStatus.empty());
}
