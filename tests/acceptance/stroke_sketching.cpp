/// Drawing with a pen: what a stroke becomes.
///
/// # Why these cases and not "it drew a line"
///
/// The classifier's job is to be PREDICTABLE, not clever. A sketcher that guesses right most of the
/// time is worse than one that never guesses, because every stroke then has to be checked. So the
/// cases below are the ones where a plausible implementation gets it wrong:
///
///   * a wobbly straight line, which a naive fit turns into an arc of enormous radius
///   * a long stroke with a slight drift, which is the same failure at a different scale
///   * an S, which no single arc passes through
///   * a clockwise arc, where the counter-clockwise convention of `addArc` silently draws the
///     complement — the long way round the circle — if the ends are not swapped
///
/// Each is a shape a hand produces constantly. None of them is exotic.
///
/// The geometry is stated in millimetres directly, with no camera and no document, because that is
/// what makes the tuning honest: the numbers in the test are the numbers a human would describe.

#include "cad/app/SketchDrawing.h"
#include "cad/app/StrokeShape.h"
#include "cad/sketch/Sketch.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <numbers>
#include <vector>

using namespace cad;
using namespace cad::app;
using Catch::Approx;

namespace {

/// Samples along a straight line, with `wobble` of noise perpendicular to it.
std::vector<StrokePoint> straight(double length, double wobble, int samples = 24) {
    std::vector<StrokePoint> out;
    for (int i = 0; i <= samples; ++i) {
        const double t = static_cast<double>(i) / samples;
        // Alternating, so the noise does not accumulate into a genuine bend.
        const double n = (i % 2 == 0 ? wobble : -wobble);
        out.push_back({length * t, n});
    }
    return out;
}

/// Samples along an arc of `radius` centred at the origin, from `from` to `to` radians.
std::vector<StrokePoint> along(double radius, double from, double to, int samples = 24) {
    std::vector<StrokePoint> out;
    for (int i = 0; i <= samples; ++i) {
        const double a = from + (to - from) * static_cast<double>(i) / samples;
        out.push_back({radius * std::cos(a), radius * std::sin(a)});
    }
    return out;
}

/// A point on a fitted arc, at one of its ends.
StrokePoint atAngle(const StrokeFit& fit, double angle) {
    return {fit.centre[0] + fit.radius * std::cos(angle), fit.centre[1] + fit.radius * std::sin(angle)};
}

constexpr double kTolerance = 0.4;   // mm, what 4 pixels comes to at a typical zoom

}   // namespace

TEST_CASE("a clean straight stroke is a line", "[stroke]") {
    const auto points = straight(100.0, 0.0);
    const auto fit = fitStroke(points, kTolerance);
    CHECK(fit.kind == StrokeKind::Line);
    CHECK(fit.start[0] == Approx(0.0));
    CHECK(fit.end[0] == Approx(100.0));
}

TEST_CASE("a wobbly straight stroke is still a line", "[stroke]") {
    // The hand shakes by a third of a millimetre. Fitting an arc through that produces a circle
    // hundreds of metres across — geometrically valid, never what anyone meant.
    const auto fit = fitStroke(straight(100.0, 0.3), kTolerance);
    CHECK(fit.kind == StrokeKind::Line);
}

TEST_CASE("a long stroke with a slight drift is a line, not a vast arc", "[stroke]") {
    // Deviation of 1.5mm exceeds the tolerance outright, so only the sagitta-to-length rule saves
    // this one: over 400mm it is a drift, not a curve. This is the case the tolerance alone gets
    // wrong, which is why the ratio exists.
    std::vector<StrokePoint> points;
    for (int i = 0; i <= 40; ++i) {
        const double t = static_cast<double>(i) / 40.0;
        points.push_back({400.0 * t, 1.5 * std::sin(std::numbers::pi * t)});
    }
    const auto fit = fitStroke(points, kTolerance);
    CHECK(fit.deviation > kTolerance);          // it really did bend more than the tolerance
    CHECK(fit.kind == StrokeKind::Line);        // and is still a line
}

TEST_CASE("a deliberate curve is an arc, with the radius drawn", "[stroke]") {
    const auto fit = fitStroke(along(50.0, 0.0, std::numbers::pi / 2), kTolerance);
    REQUIRE(fit.kind == StrokeKind::Arc);
    CHECK(fit.radius == Approx(50.0).margin(0.5));
    CHECK(fit.centre[0] == Approx(0.0).margin(0.5));
    CHECK(fit.centre[1] == Approx(0.0).margin(0.5));
}

TEST_CASE("a clockwise stroke keeps its own endpoints", "[stroke]") {
    // `Sketch::addArc` sweeps counter-clockwise, so a clockwise stroke has to come back with its
    // ends swapped. Get this wrong and the arc drawn is the COMPLEMENT — the long way round the
    // circle — which is silent, spectacular, and only visible once the profile is extruded.
    const auto points = along(50.0, std::numbers::pi / 2, 0.0);
    const auto fit = fitStroke(points, kTolerance);
    REQUIRE(fit.kind == StrokeKind::Arc);

    // Whichever way the ends were stored, the arc must pass through where the hand started and
    // finished — and the SHORT way, so the swept angle is the quarter turn drawn.
    const auto a = atAngle(fit, fit.startAngle);
    const auto b = atAngle(fit, fit.endAngle);
    const double toStart = std::min(std::hypot(a[0] - points.front()[0], a[1] - points.front()[1]),
                                    std::hypot(b[0] - points.front()[0], b[1] - points.front()[1]));
    const double toEnd = std::min(std::hypot(a[0] - points.back()[0], a[1] - points.back()[1]),
                                  std::hypot(b[0] - points.back()[0], b[1] - points.back()[1]));
    CHECK(toStart < 0.5);
    CHECK(toEnd < 0.5);

    double swept = fit.endAngle - fit.startAngle;
    while (swept < 0.0) swept += 2.0 * std::numbers::pi;
    CHECK(swept == Approx(std::numbers::pi / 2).margin(0.1));
}

TEST_CASE("an S-shaped stroke is not fitted to a single arc", "[stroke]") {
    // No arc passes through a stroke that bends both ways. The honest answers are a spline or a
    // line; a line is chosen because the user can chain another stroke onto its endpoint, whereas a
    // wrong arc has to be found and deleted.
    std::vector<StrokePoint> points;
    for (int i = 0; i <= 40; ++i) {
        const double t = static_cast<double>(i) / 40.0;
        points.push_back({100.0 * t, 8.0 * std::sin(2.0 * std::numbers::pi * t)});
    }
    const auto fit = fitStroke(points, kTolerance);
    CHECK(fit.kind == StrokeKind::Line);
}

TEST_CASE("a tap is not a stroke", "[stroke]") {
    const std::vector<StrokePoint> one{{10.0, 10.0}};
    CHECK(fitStroke(one, kTolerance).kind == StrokeKind::Nothing);

    // Ending where it began: no chord, so nothing can be measured against it. Refused rather than
    // guessed into a circle this function has no way to verify.
    std::vector<StrokePoint> loop = along(20.0, 0.0, 2.0 * std::numbers::pi);
    CHECK(fitStroke(loop, kTolerance).kind == StrokeKind::Nothing);
}

// ── through SketchDrawing, where the geometry actually lands ─────────────────────────────

TEST_CASE("strokes chain, and each join is a real constraint", "[stroke]") {
    sketch::Sketch sketch;
    SketchDrawing drawing;
    drawing.setTool(SketchDrawing::Tool::Line);
    SketchDrawing::Context ctx{&sketch, 0.1, units::UnitSystem::Millimetre};

    const std::vector<StrokePoint> first{{0, 0}, {50, 0}, {100, 0}};
    // Deliberately started a whole millimetre away from where the first ended. A hand does this
    // every time; the chain must survive it.
    const std::vector<StrokePoint> second{{101, 1}, {100, 30}, {100, 60}};

    REQUIRE(drawing.stroke(ctx, first).geometryChanged);
    REQUIRE(drawing.stroke(ctx, second).geometryChanged);

    REQUIRE(sketch.geometry().size() == 2);
    // Coincident, not merely touching. Snapping makes two points share coordinates today; the
    // constraint is what keeps them together when the solver moves something tomorrow.
    const auto& constraints = sketch.constraints();
    const auto joins = std::count_if(constraints.begin(), constraints.end(), [](const auto& c) {
        return c.kind == sketch::ConstraintKind::Coincident;
    });
    CHECK(joins == 1);

    // And the second segment starts where the first ended, not at the point the hand reported.
    CHECK(sketch.geometry()[1].p[0] == Approx(100.0));
    CHECK(sketch.geometry()[1].p[1] == Approx(0.0));
}

TEST_CASE("a curved stroke lands as an arc in the sketch", "[stroke]") {
    sketch::Sketch sketch;
    SketchDrawing drawing;
    drawing.setTool(SketchDrawing::Tool::Line);   // ONE tool: the stroke decides, not the toolbar
    SketchDrawing::Context ctx{&sketch, 0.1, units::UnitSystem::Millimetre};

    REQUIRE(drawing.stroke(ctx, along(40.0, 0.0, std::numbers::pi / 2)).geometryChanged);
    REQUIRE(sketch.geometry().size() == 1);
    CHECK(sketch.geometry().front().kind == sketch::GeoKind::Arc);
    CHECK(sketch.geometry().front().p[2] == Approx(40.0).margin(0.5));
}

TEST_CASE("a straight stroke is inferred horizontal", "[stroke]") {
    sketch::Sketch sketch;
    SketchDrawing drawing;
    drawing.setTool(SketchDrawing::Tool::Line);
    SketchDrawing::Context ctx{&sketch, 0.1, units::UnitSystem::Millimetre};

    // Drawn a degree or so off horizontal, as a hand does.
    const std::vector<StrokePoint> points{{0, 0}, {40, 0.3}, {80, 0.6}};
    REQUIRE(drawing.stroke(ctx, points).geometryChanged);

    const auto& constraints = sketch.constraints();
    CHECK(std::any_of(constraints.begin(), constraints.end(), [](const auto& c) {
        return c.kind == sketch::ConstraintKind::Horizontal;
    }));
}

TEST_CASE("ending a chain does not link the next run to the last one", "[stroke]") {
    sketch::Sketch sketch;
    SketchDrawing drawing;
    drawing.setTool(SketchDrawing::Tool::Line);
    SketchDrawing::Context ctx{&sketch, 0.1, units::UnitSystem::Millimetre};

    drawing.stroke(ctx, std::vector<StrokePoint>{{0, 0}, {100, 0}});
    drawing.endChain();
    drawing.stroke(ctx, std::vector<StrokePoint>{{200, 200}, {300, 200}});

    // An invisible constraint between two shapes that look separate is worse than a missing one:
    // the solver enforces it and the user cannot see why their sketch moves.
    const auto& constraints = sketch.constraints();
    CHECK(std::none_of(constraints.begin(), constraints.end(), [](const auto& c) {
        return c.kind == sketch::ConstraintKind::Coincident;
    }));
}

// ── tangency ────────────────────────────────────────────────────────────────────────────

TEST_CASE("an arc drawn onto a line is constrained tangent, and the solver honours it",
          "[stroke][tangent]") {
    // The constraint that makes a fillet behave like a fillet. Without it the arc keeps whatever
    // angle the hand happened to leave, and the corner un-rounds itself the first time a dimension
    // moves.
    sketch::Sketch sketch;
    SketchDrawing drawing;
    drawing.setTool(SketchDrawing::Tool::Line);
    SketchDrawing::Context ctx{&sketch, 0.1, units::UnitSystem::Millimetre};

    // A horizontal line, then an arc curving up away from its end.
    REQUIRE(drawing.stroke(ctx, std::vector<StrokePoint>{{0, 0}, {30, 0}, {60, 0}}).geometryChanged);
    std::vector<StrokePoint> curve;
    for (int i = 0; i <= 16; ++i) {
        const double a = -std::numbers::pi / 2 + (std::numbers::pi / 2) * i / 16.0;
        curve.push_back({60.0 + 20.0 * std::cos(a), 20.0 + 20.0 * std::sin(a)});
    }
    REQUIRE(drawing.stroke(ctx, curve).geometryChanged);

    REQUIRE(sketch.geometry().size() == 2);
    REQUIRE(sketch.geometry()[1].kind == sketch::GeoKind::Arc);

    const auto& constraints = sketch.constraints();
    CHECK(std::any_of(constraints.begin(), constraints.end(), [](const auto& c) {
        return c.kind == sketch::ConstraintKind::Tangent;
    }));

    // And it is ENFORCED, which "solved with no conflicts" does not prove: a sketch that was
    // already tangent when drawn solves cleanly whether or not the constraint exists. So the line
    // is DRIVEN to a different length first, and tangency is then measured at the join.
    //
    // This is the difference between testing that a constraint was recorded and testing that it
    // does anything.
    const auto line = sketch.ids()[0];
    const auto arc = sketch.ids()[1];
    sketch.distance(line, sketch::PointRef::Start, line, sketch::PointRef::End, 90.0);

    const auto report = sketch.solve();
    INFO(report.message);
    REQUIRE(report.solved);
    CHECK(report.conflicting.empty());

    const auto* solvedLine = sketch.find(line);
    const auto* solvedArc = sketch.find(arc);
    REQUIRE(solvedLine != nullptr);
    REQUIRE(solvedArc != nullptr);

    // The arc's tangent at a point is perpendicular to its radius there. Compare its direction with
    // the line's; parallel means the cross product is zero.
    const auto join = sketch.pointAt(arc, sketch::PointRef::Start);
    REQUIRE(join);
    const double rx = join.value()[0] - solvedArc->p[0];
    const double ry = join.value()[1] - solvedArc->p[1];
    const double tx = -ry, ty = rx;                       // tangent = radius rotated 90 degrees
    const double lx = solvedLine->p[2] - solvedLine->p[0];
    const double ly = solvedLine->p[3] - solvedLine->p[1];

    const double cross = std::abs(tx * ly - ty * lx)
                         / (std::hypot(tx, ty) * std::hypot(lx, ly));
    INFO("sin of the angle at the join: " << cross);
    CHECK(cross < 0.02);   // within about a degree
}

TEST_CASE("two straight strokes are not made tangent to each other", "[stroke][tangent]") {
    // Tangency between two lines means they are one line. Applying it at every join would flatten
    // corners the user drew on purpose — so it is the arc that asks for it, not the join.
    sketch::Sketch sketch;
    SketchDrawing drawing;
    drawing.setTool(SketchDrawing::Tool::Line);
    SketchDrawing::Context ctx{&sketch, 0.1, units::UnitSystem::Millimetre};

    drawing.stroke(ctx, std::vector<StrokePoint>{{0, 0}, {50, 0}});
    drawing.stroke(ctx, std::vector<StrokePoint>{{50, 0}, {50, 40}});

    const auto& constraints = sketch.constraints();
    CHECK(std::none_of(constraints.begin(), constraints.end(), [](const auto& c) {
        return c.kind == sketch::ConstraintKind::Tangent;
    }));
    const auto report = sketch.solve();
    INFO(report.message);
    CHECK(report.solved);
}
