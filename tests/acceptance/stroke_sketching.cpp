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

TEST_CASE("closing a profile constrains it closed", "[stroke]") {
    // A run of four strokes back to the start has FOUR corners, so it needs four coincidences. It
    // used to get three: each segment was joined to the one before it and nothing tied the last
    // endpoint to the first. The profile looked closed and came apart on the first dimension edit,
    // after which Extrude refuses with nothing on screen to explain why.
    sketch::Sketch sketch;
    SketchDrawing drawing;
    drawing.setTool(SketchDrawing::Tool::Line);
    SketchDrawing::Context ctx{&sketch, 0.1, units::UnitSystem::Millimetre};

    drawing.stroke(ctx, std::vector<StrokePoint>{{0, 0}, {50, 0}, {100, 0}});
    drawing.stroke(ctx, std::vector<StrokePoint>{{100, 0}, {100, 30}, {100, 60}});
    drawing.stroke(ctx, std::vector<StrokePoint>{{100, 60}, {50, 60}, {0, 60}});
    drawing.stroke(ctx, std::vector<StrokePoint>{{0, 60}, {0, 30}, {0, 0}});

    REQUIRE(sketch.geometry().size() == 4);
    const auto& constraints = sketch.constraints();
    const auto joins = std::count_if(constraints.begin(), constraints.end(), [](const auto& c) {
        return c.kind == sketch::ConstraintKind::Coincident;
    });
    CHECK(joins == 4);

    const auto report = sketch.solve();
    INFO(report.message);
    CHECK(report.solved);
}

TEST_CASE("the circle tool draws a circle from a stroke", "[stroke]") {
    sketch::Sketch sketch;
    SketchDrawing drawing;
    drawing.setTool(SketchDrawing::Tool::Circle);
    SketchDrawing::Context ctx{&sketch, 0.1, units::UnitSystem::Millimetre};

    // Centre outwards, the same thing the two-click form means.
    REQUIRE(drawing.stroke(ctx, std::vector<StrokePoint>{{0, 0}, {20, 0}, {40, 0}}).geometryChanged);
    REQUIRE(sketch.geometry().size() == 1);
    CHECK(sketch.geometry().front().kind == sketch::GeoKind::Circle);
    CHECK(sketch.geometry().front().p[2] == Approx(40.0));
}

TEST_CASE("a locked length applies to a stroke as it does to a click", "[stroke]") {
    sketch::Sketch sketch;
    SketchDrawing drawing;
    drawing.setTool(SketchDrawing::Tool::Line);
    SketchDrawing::Context ctx{&sketch, 0.1, units::UnitSystem::Millimetre};

    // Mid-chain, which is the only time a lock means anything: Tab locks the size of the segment
    // being drawn, so there has to be one in progress.
    REQUIRE(drawing.stroke(ctx, std::vector<StrokePoint>{{0, 0}, {30, 0}, {60, 0}}).geometryChanged);
    drawing.type('4');
    drawing.type('0');
    REQUIRE(drawing.lock(ctx));

    // Drawn 100mm long with 40mm locked: the hand chose the direction, Tab chose the size.
    REQUIRE(drawing.stroke(ctx, std::vector<StrokePoint>{{60, 0}, {60, 50}, {60, 100}}).geometryChanged);
    REQUIRE(sketch.geometry().size() == 2);
    const auto& line = sketch.geometry()[1];
    CHECK(std::hypot(line.p[2] - line.p[0], line.p[3] - line.p[1]) == Approx(40.0));

    // And DRIVING, so the solver keeps it rather than treating it as a coincidence.
    const auto& constraints = sketch.constraints();
    CHECK(std::any_of(constraints.begin(), constraints.end(), [](const auto& c) {
        return c.kind == sketch::ConstraintKind::Distance && c.value == Approx(40.0);
    }));
}

// ── the rectangle tool ──────────────────────────────────────────────────────────────────

TEST_CASE("a rectangle is four lines that stay rectangular", "[stroke][rectangle]") {
    // The point of the tool is not the four lines — anyone can draw those — it is the constraints
    // that come with them. Two horizontal, two vertical, four coincident: enough that the shape
    // stays a rectangle when a dimension moves, which four hand-drawn lines do not.
    sketch::Sketch sketch;
    SketchDrawing drawing;
    drawing.setTool(SketchDrawing::Tool::Rectangle);
    SketchDrawing::Context ctx{&sketch, 0.1, units::UnitSystem::Millimetre};

    // ONE drag, corner to corner.
    REQUIRE(drawing.stroke(ctx, std::vector<StrokePoint>{{0, 0}, {50, 20}, {100, 60}})
                .geometryChanged);

    REQUIRE(sketch.geometry().size() == 4);
    const auto& constraints = sketch.constraints();
    const auto count = [&](sketch::ConstraintKind kind) {
        return std::count_if(constraints.begin(), constraints.end(),
                             [kind](const auto& c) { return c.kind == kind; });
    };
    CHECK(count(sketch::ConstraintKind::Horizontal) == 2);
    CHECK(count(sketch::ConstraintKind::Vertical) == 2);
    CHECK(count(sketch::ConstraintKind::Coincident) == 4);

    const auto report = sketch.solve();
    INFO(report.message);
    CHECK(report.solved);
    CHECK(report.conflicting.empty());
}

TEST_CASE("a rectangle closes, so it can be extruded", "[stroke][rectangle]") {
    // The assertion that matters to a user. A profile that merely LOOKS closed produces a wire and
    // not a face, and the first they hear of it is Extrude refusing.
    sketch::Sketch sketch;
    SketchDrawing drawing;
    drawing.setTool(SketchDrawing::Tool::Rectangle);
    SketchDrawing::Context ctx{&sketch, 0.1, units::UnitSystem::Millimetre};

    REQUIRE(drawing.stroke(ctx, std::vector<StrokePoint>{{0, 0}, {80, 40}}).geometryChanged);
    sketch.solve();

    const auto face = sketch.toFace();
    if (!face) INFO(face.error().message);
    REQUIRE(face);
    CHECK(face.value().volume() == Approx(0.0));   // a face encloses no volume
}

TEST_CASE("a degenerate rectangle is refused, not drawn", "[stroke][rectangle]") {
    // A drag along one axis has no second dimension. Four zero-width lines are geometry the user
    // can neither see nor select, and the only way to be rid of them is the model tree.
    sketch::Sketch sketch;
    SketchDrawing drawing;
    drawing.setTool(SketchDrawing::Tool::Rectangle);
    SketchDrawing::Context ctx{&sketch, 0.1, units::UnitSystem::Millimetre};

    const auto flat = drawing.stroke(ctx, std::vector<StrokePoint>{{0, 0}, {50, 0}, {100, 0}});
    CHECK_FALSE(flat.geometryChanged);
    CHECK(sketch.geometry().empty());
    CHECK_FALSE(flat.status.empty());   // and it says why
}

TEST_CASE("two clicks also make a rectangle", "[stroke][rectangle]") {
    // A mouse points; a pen drags. Both reach the same tool, as they do for lines.
    sketch::Sketch sketch;
    SketchDrawing drawing;
    drawing.setTool(SketchDrawing::Tool::Rectangle);
    SketchDrawing::Context ctx{&sketch, 0.1, units::UnitSystem::Millimetre};

    drawing.click(ctx, {10, 10});
    const auto second = drawing.click(ctx, {70, 50});
    CHECK(second.geometryChanged);
    CHECK(sketch.geometry().size() == 4);
}

/// Locks `text` as the pending dimension, having started a rectangle at `corner`.
///
/// The order matters and is not obvious: `type()` refuses a digit with nothing pending, because a
/// digit typed at rest is a shortcut rather than a dimension. So the first corner has to be placed
/// before the size can be typed — which is exactly the order a user works in.
void lockAt(SketchDrawing& drawing, const SketchDrawing::Context& ctx,
            SketchDrawing::Point corner, const char* text) {
    drawing.click(ctx, corner);
    for (const char* c = text; *c != '\0'; ++c) REQUIRE(drawing.type(*c));
    REQUIRE(drawing.lock(ctx));
}

/// The bounding diagonal of everything in the sketch, measured after solving.
double diagonalOf(const sketch::Sketch& sketch) {
    double minX = 1e9, maxX = -1e9, minY = 1e9, maxY = -1e9;
    for (const auto& g : sketch.geometry()) {
        for (int i = 0; i < 4; i += 2) {
            minX = std::min(minX, g.p[i]);
            maxX = std::max(maxX, g.p[i]);
            minY = std::min(minY, g.p[i + 1]);
            maxY = std::max(maxY, g.p[i + 1]);
        }
    }
    return std::hypot(maxX - minX, maxY - minY);
}

TEST_CASE("a locked rectangle is CONSTRAINED to its size, not merely drawn at it", "[stroke][rectangle]") {
    // The click path already scaled the far corner to the locked length — the generic branch above
    // the tools does that for every tool. What it never did was record the size as a constraint, and
    // endChain() then threw the lock away. So the rectangle came out the right size and nothing held
    // it there: the first solve that had any reason to move it could.
    //
    // The DIAGONAL, because that is the number `measure()` reports for this tool, and its own comment
    // says the rubber band and the result must not disagree. The drag chooses the proportions.
    sketch::Sketch sketch;
    SketchDrawing drawing;
    drawing.setTool(SketchDrawing::Tool::Rectangle);
    SketchDrawing::Context ctx{&sketch, 0.1, units::UnitSystem::Millimetre};

    // A 3-4-5 drag, so 30 by 40 is a diagonal of 50 and a locked 100 is an exact doubling.
    lockAt(drawing, ctx, {0, 0}, "100");
    REQUIRE(drawing.click(ctx, {30, 40}).geometryChanged);
    REQUIRE(sketch.geometry().size() == 4);

    const auto& constraints = sketch.constraints();
    CHECK(std::any_of(constraints.begin(), constraints.end(), [](const auto& c) {
        // DRIVING, for the reason the line path gives: a locked length the solver may undo was never
        // locked.
        return c.kind == sketch::ConstraintKind::Distance && c.value == Approx(100.0);
    }));

    const auto report = sketch.solve();
    INFO(report.message);
    CHECK(report.solved);
    // Measured after solving, because it is the solver that has to agree, not the placement.
    CHECK(diagonalOf(sketch) == Approx(100.0).margin(1e-6));
}

TEST_CASE("a stroked rectangle honours the lock too", "[stroke][rectangle]") {
    // The stroke path ignored the lock ENTIRELY — its rectangle branch returns before the code that
    // applies a locked length — so a stylus drag came out whatever size the hand made it while the
    // shell went on showing a padlock. That is the "the padlock meant nothing" bug already fixed one
    // tool along, and this is the same tool's other input path.
    sketch::Sketch sketch;
    SketchDrawing drawing;
    drawing.setTool(SketchDrawing::Tool::Rectangle);
    SketchDrawing::Context ctx{&sketch, 0.1, units::UnitSystem::Millimetre};

    lockAt(drawing, ctx, {0, 0}, "100");
    REQUIRE(drawing.stroke(ctx, std::vector<StrokePoint>{{0, 0}, {15, 20}, {30, 40}})
                .geometryChanged);
    REQUIRE(sketch.geometry().size() == 4);

    CHECK(sketch.solve().solved);
    CHECK(diagonalOf(sketch) == Approx(100.0).margin(1e-6));
}

TEST_CASE("a rectangle drawn without a lock is not constrained to a size", "[stroke][rectangle]") {
    // The other half: honouring a lock must not mean inventing a dimension for every rectangle. One
    // drawn freehand keeps the freedom to be resized by dragging a corner later.
    sketch::Sketch sketch;
    SketchDrawing drawing;
    drawing.setTool(SketchDrawing::Tool::Rectangle);
    SketchDrawing::Context ctx{&sketch, 0.1, units::UnitSystem::Millimetre};

    REQUIRE(drawing.stroke(ctx, std::vector<StrokePoint>{{0, 0}, {50, 25}, {100, 50}})
                .geometryChanged);
    const auto& constraints = sketch.constraints();
    CHECK(std::none_of(constraints.begin(), constraints.end(), [](const auto& c) {
        return c.kind == sketch::ConstraintKind::Distance;
    }));
    CHECK(sketch.solve().solved);
}

TEST_CASE("the rectangle tool previews the shape it will draw", "[stroke][rectangle]") {
    // Drawn blind otherwise: the user drags and sees nothing until they let go, which is the one
    // thing a rubber band exists to prevent. The preview must be the SHAPE, not the diagonal —
    // showing the drag vector tells you where the corner is and nothing about the rectangle.
    sketch::Sketch sketch;
    SketchDrawing drawing;
    drawing.setTool(SketchDrawing::Tool::Rectangle);
    SketchDrawing::Context ctx{&sketch, 0.1, units::UnitSystem::Millimetre};

    drawing.click(ctx, {0, 0});          // first corner
    drawing.hover(ctx, {60, 40});        // dragging towards the second

    const auto preview = drawing.previewSegments(ctx);
    REQUIRE_FALSE(preview.empty());

    // Every preview point lies on the rectangle's outline: on one of the two vertical sides, or on
    // one of the two horizontal ones. A diagonal band would fail this immediately.
    for (const auto& p : preview) {
        const bool onVertical = std::abs(p[0] - 0.0) < 1e-6 || std::abs(p[0] - 60.0) < 1e-6;
        const bool onHorizontal = std::abs(p[1] - 0.0) < 1e-6 || std::abs(p[1] - 40.0) < 1e-6;
        INFO("preview point " << p[0] << "," << p[1]);
        CHECK((onVertical || onHorizontal));
    }

    // And the live readout says both sizes. One number cannot describe a rectangle, and showing
    // only one is worse than none: it reads as "the size".
    const auto measured = drawing.measure();
    CHECK(measured.rectangle);
    CHECK(measured.width == Approx(60.0));
    CHECK(measured.height == Approx(40.0));

    const auto text = drawing.text(units::UnitSystem::Millimetre);
    CHECK(text.valid);
    INFO("readout: " << text.length);
    CHECK(text.length.find("x") != std::string::npos);
    CHECK(text.angle.empty());   // a rectangle has no angle to report
}

TEST_CASE("a short wobbly stroke is a line, not a huge arc", "[stroke]") {
    // Reported from the iPad: "random curves and circles get sketched when I try to draw a line".
    //
    // Neither of the other two rules protects a SHORT stroke. Five millimetres of wobble over fifty
    // is more than the straightness tolerance and more than 2% of the chord, so a circle was fitted
    // — one of enormous radius, drawn as a wild curve across the sketch. The user then has to
    // notice each one and delete it, which is worse than the tool refusing to draw at all.
    std::vector<StrokePoint> points;
    for (int i = 0; i <= 12; ++i) {
        const double t = static_cast<double>(i) / 12.0;
        // A single gentle bow, which is the shape a hand actually makes — alternating noise would
        // be caught by the S-curve rule instead and prove nothing.
        points.push_back({50.0 * t, 2.0 * std::sin(std::numbers::pi * t)});
    }
    const auto fit = fitStroke(points, kTolerance);
    INFO("deviation " << fit.deviation << " over a chord of 50");
    CHECK(fit.kind == StrokeKind::Line);
}

TEST_CASE("an arc has to turn far enough to be one", "[stroke]") {
    // The boundary, stated as angles because that is what the rule is about. Ten degrees of sweep is
    // a hand not holding still; sixty is a deliberate curve.
    const auto barely = fitStroke(along(200.0, 0.0, 10.0 * std::numbers::pi / 180.0), kTolerance);
    CHECK(barely.kind == StrokeKind::Line);

    const auto deliberate = fitStroke(along(50.0, 0.0, 60.0 * std::numbers::pi / 180.0), kTolerance);
    CHECK(deliberate.kind == StrokeKind::Arc);
    CHECK(deliberate.radius == Approx(50.0).margin(1.0));
}
