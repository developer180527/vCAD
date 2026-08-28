#include "cad/sketch/Trim.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace cad::sketch {
namespace {

using Point = std::array<double, 2>;
using kernel::Error;
using kernel::ErrorCode;

/// Below this, two points are the same point and a survivor is not worth keeping.
///
/// In sketch units, which are millimetres. A survivor shorter than a micron is geometry the user
/// can neither see nor select, and leaving one behind after a trim is indistinguishable from the
/// trim having gone wrong.
constexpr double kEpsilon = 1e-6;

double normalisePositive(double angle) {
    constexpr double kTwoPi = 2.0 * std::numbers::pi;
    angle = std::fmod(angle, kTwoPi);
    return angle < 0.0 ? angle + kTwoPi : angle;
}

/// The angular sweep of an arc, from its start towards its end, always positive.
///
/// Arcs are stored as a start and an end angle and drawn counter-clockwise between them, so a
/// "negative" arc is one that wraps past zero rather than one that runs backwards.
double sweepOf(const Geometry& g) {
    if (g.kind == GeoKind::Circle) return 2.0 * std::numbers::pi;
    const double sweep = normalisePositive(g.p[4] - g.p[3]);
    // A stored arc whose ends coincide is a full circle, not an empty one: an arc of zero sweep
    // could never have been drawn.
    return sweep < kEpsilon ? 2.0 * std::numbers::pi : sweep;
}

double startAngleOf(const Geometry& g) { return g.kind == GeoKind::Circle ? 0.0 : g.p[3]; }

Point pointOnLine(const Geometry& g, double t) {
    return {g.p[0] + (g.p[2] - g.p[0]) * t, g.p[1] + (g.p[3] - g.p[1]) * t};
}

/// Where a point falls along a curve: 0..1 for a line, 0..sweep for a circle or arc.
///
/// Returns nothing when the point is not ON the curve — off the end of a segment, or outside an
/// arc's range. That filtering is what makes `intersections` mean "these curves actually cross"
/// rather than "their infinite extensions would".
std::optional<double> parameterOf(const Geometry& g, Point at, double tolerance) {
    if (g.kind == GeoKind::Line) {
        const double dx = g.p[2] - g.p[0];
        const double dy = g.p[3] - g.p[1];
        const double lengthSq = dx * dx + dy * dy;
        if (lengthSq < kEpsilon * kEpsilon) return std::nullopt;
        const double t = ((at[0] - g.p[0]) * dx + (at[1] - g.p[1]) * dy) / lengthSq;
        // Slack in PARAMETER terms, scaled from the tolerance so a short line is not judged more
        // harshly than a long one.
        const double slack = tolerance / std::sqrt(lengthSq);
        if (t < -slack || t > 1.0 + slack) return std::nullopt;
        return std::clamp(t, 0.0, 1.0);
    }
    if (g.kind == GeoKind::Circle || g.kind == GeoKind::Arc) {
        if (g.p[2] < kEpsilon) return std::nullopt;
        const double u = normalisePositive(std::atan2(at[1] - g.p[1], at[0] - g.p[0])
                                           - startAngleOf(g));
        const double sweep = sweepOf(g);
        if (u > sweep) {
            // Past the end, but a point just before the START is also just past the end once
            // wrapped -- so let it back in when it is within tolerance of either.
            const double gap = 2.0 * std::numbers::pi - u;
            const double slack = tolerance / g.p[2];
            if (gap < slack) return 0.0;
            if (u - sweep < slack) return sweep;
            return std::nullopt;
        }
        return u;
    }
    return std::nullopt;
}

void addIfOnBoth(std::vector<Point>& out, const Geometry& a, const Geometry& b, Point p) {
    if (!parameterOf(a, p, kEpsilon * 10.0) || !parameterOf(b, p, kEpsilon * 10.0)) return;
    // Deduplicated: two curves meeting tangentially produce the same root twice, and a doubled
    // cut point would make an empty span between itself and itself.
    for (const Point& seen : out) {
        if (std::hypot(seen[0] - p[0], seen[1] - p[1]) < kEpsilon * 10.0) return;
    }
    out.push_back(p);
}

void lineLine(std::vector<Point>& out, const Geometry& a, const Geometry& b) {
    const double ax = a.p[0], ay = a.p[1], adx = a.p[2] - a.p[0], ady = a.p[3] - a.p[1];
    const double bx = b.p[0], by = b.p[1], bdx = b.p[2] - b.p[0], bdy = b.p[3] - b.p[1];
    const double denominator = adx * bdy - ady * bdx;
    if (std::abs(denominator) < 1e-15) return;   // parallel, including collinear
    const double t = ((bx - ax) * bdy - (by - ay) * bdx) / denominator;
    addIfOnBoth(out, a, b, Point{ax + adx * t, ay + ady * t});
}

/// A line against a circle or an arc: substitute the line into the circle equation.
void lineCircle(std::vector<Point>& out, const Geometry& line, const Geometry& circle) {
    const double dx = line.p[2] - line.p[0];
    const double dy = line.p[3] - line.p[1];
    const double fx = line.p[0] - circle.p[0];
    const double fy = line.p[1] - circle.p[1];

    const double a = dx * dx + dy * dy;
    if (a < kEpsilon * kEpsilon) return;
    const double b = 2.0 * (fx * dx + fy * dy);
    const double c = fx * fx + fy * fy - circle.p[2] * circle.p[2];

    const double discriminant = b * b - 4.0 * a * c;
    if (discriminant < 0.0) return;
    const double root = std::sqrt(discriminant);
    for (const double t : {(-b - root) / (2.0 * a), (-b + root) / (2.0 * a)}) {
        addIfOnBoth(out, line, circle, Point{line.p[0] + dx * t, line.p[1] + dy * t});
    }
}

/// Two circles (or arcs): the radical line through their intersection points.
void circleCircle(std::vector<Point>& out, const Geometry& a, const Geometry& b) {
    const double dx = b.p[0] - a.p[0];
    const double dy = b.p[1] - a.p[1];
    const double d = std::hypot(dx, dy);
    if (d < kEpsilon) return;                                  // concentric
    if (d > a.p[2] + b.p[2] + kEpsilon) return;                // too far apart
    if (d < std::abs(a.p[2] - b.p[2]) - kEpsilon) return;      // one inside the other

    const double base = (d * d + a.p[2] * a.p[2] - b.p[2] * b.p[2]) / (2.0 * d);
    const double heightSq = a.p[2] * a.p[2] - base * base;
    const double height = heightSq > 0.0 ? std::sqrt(heightSq) : 0.0;

    const double mx = a.p[0] + base * dx / d;
    const double my = a.p[1] + base * dy / d;
    addIfOnBoth(out, a, b, Point{mx + height * dy / d, my - height * dx / d});
    addIfOnBoth(out, a, b, Point{mx - height * dy / d, my + height * dx / d});
}

bool isRound(const Geometry& g) {
    return g.kind == GeoKind::Circle || g.kind == GeoKind::Arc;
}

/// Whether a constraint pins a POINT of `id` rather than describing its shape.
///
/// A trim moves endpoints by definition, so a pinned one would be dragged back by the next solve
/// or report the sketch over-constrained -- and both look like the trim having failed. Shape
/// constraints survive: a shortened line is still horizontal.
bool pinsAPointOf(const Constraint& c, GeoId id) {
    if (c.a != id && c.b != id) return false;
    switch (c.kind) {
        case ConstraintKind::Coincident:
        case ConstraintKind::Distance:
        case ConstraintKind::PointOnLine:
        case ConstraintKind::LockX:
        case ConstraintKind::LockY:
        case ConstraintKind::Tangent:
            return true;
        default:
            return false;
    }
}

}  // namespace

std::vector<Point> intersections(const Geometry& a, const Geometry& b) {
    std::vector<Point> out;
    if (a.kind == GeoKind::Point || b.kind == GeoKind::Point) return out;

    if (a.kind == GeoKind::Line && b.kind == GeoKind::Line) {
        lineLine(out, a, b);
    } else if (a.kind == GeoKind::Line && isRound(b)) {
        lineCircle(out, a, b);
    } else if (isRound(a) && b.kind == GeoKind::Line) {
        lineCircle(out, b, a);
    } else if (isRound(a) && isRound(b)) {
        circleCircle(out, a, b);
    }
    return out;
}

kernel::Result<TrimResult> trim(Sketch& sketch, GeoId target, Point at) {
    const Geometry* found = sketch.find(target);
    if (found == nullptr) {
        return Error{ErrorCode::InvalidInput, "That geometry is not in this sketch."};
    }
    if (found->kind == GeoKind::Point) {
        return Error{ErrorCode::InvalidInput, "A point has no length to trim."};
    }
    const Geometry original = *found;

    // Where the click falls along the curve. Refused rather than guessed: a click nowhere near the
    // curve cannot say which span was meant, and picking one would delete something at random.
    const auto clickParameter = parameterOf(original, at, std::max(1.0, original.p[2]));
    if (!clickParameter) {
        return Error{ErrorCode::InvalidInput, "Click on the part of the curve to remove."};
    }

    // Every point where something else crosses it, as parameters along this curve.
    std::vector<double> cuts;
    for (std::size_t i = 0; i < sketch.geometry().size() && i < sketch.ids().size(); ++i) {
        if (sketch.ids()[i] == target) continue;
        for (const Point& p : intersections(original, sketch.geometry()[i])) {
            if (const auto u = parameterOf(original, p, kEpsilon * 10.0)) cuts.push_back(*u);
        }
    }
    std::sort(cuts.begin(), cuts.end());
    cuts.erase(std::unique(cuts.begin(), cuts.end(),
                           [](double x, double y) { return std::abs(x - y) < kEpsilon; }),
               cuts.end());

    TrimResult result;
    const auto dropPointConstraints = [&sketch, target, &result] {
        for (std::size_t i = sketch.constraints().size(); i > 0; --i) {
            if (pinsAPointOf(sketch.constraints()[i - 1], target)) {
                sketch.removeConstraint(i - 1);
                ++result.constraintsDropped;
            }
        }
    };

    const bool round = isRound(original);
    const double sweep = round ? sweepOf(original) : 1.0;
    const bool closed = round && original.kind == GeoKind::Circle;

    // A closed circle is a ring: its spans wrap, so it needs two cuts before any span has both ends.
    if (closed && cuts.size() < 2) {
        sketch.removeGeometry(target);
        result.removedWhole = true;
        return result;
    }

    if (closed) {
        // Find the pair of cuts the click falls between, wrapping past the seam, and keep the REST
        // of the ring as an arc running the other way round.
        const double click = *clickParameter;
        std::size_t after = 0;
        while (after < cuts.size() && cuts[after] < click) ++after;
        const double hi = after < cuts.size() ? cuts[after] : cuts.front();
        const double lo = after > 0 ? cuts[after - 1] : cuts.back();

        Geometry arc = original;
        arc.kind = GeoKind::Arc;
        arc.p[3] = normalisePositive(hi);
        arc.p[4] = normalisePositive(lo);
        if (normalisePositive(arc.p[4] - arc.p[3]) < kEpsilon) {
            sketch.removeGeometry(target);
            result.removedWhole = true;
            return result;
        }
        sketch.replaceGeometry(target, arc);
        dropPointConstraints();
        return result;
    }

    // Open curve: a line or an arc, parameterised 0..1 or 0..sweep.
    const double click = *clickParameter;
    std::optional<double> lo;
    std::optional<double> hi;
    for (const double cut : cuts) {
        if (cut < click - kEpsilon) lo = cut;
        if (cut > click + kEpsilon && !hi) hi = cut;
    }

    const double head = lo.value_or(0.0);                 // survivor before the removed span
    const double tail = hi.value_or(round ? sweep : 1.0); // survivor after it
    const bool keepHead = lo.has_value() && head > kEpsilon;
    const bool keepTail = hi.has_value() && (round ? sweep - tail : 1.0 - tail) > kEpsilon;

    if (!keepHead && !keepTail) {
        // Nothing cuts it, or the cuts sit at its very ends -- which is a chain segment joined to
        // its neighbours, where the span between the joins IS the whole curve.
        sketch.removeGeometry(target);
        result.removedWhole = true;
        return result;
    }

    const auto shrink = [&](double from, double to) {
        Geometry g = original;
        if (round) {
            g.kind = GeoKind::Arc;
            g.p[3] = startAngleOf(original) + from;
            g.p[4] = startAngleOf(original) + to;
        } else {
            const Point a = pointOnLine(original, from);
            const Point b = pointOnLine(original, to);
            g.p = {a[0], a[1], b[0], b[1], 0.0};
        }
        return g;
    };

    if (keepHead && keepTail) {
        // A span in the middle: the original id keeps the FIRST piece, so its shape constraints
        // stay with something rather than being dropped along with a discarded id.
        sketch.replaceGeometry(target, shrink(0.0, head));
        const Geometry second = shrink(tail, round ? sweep : 1.0);
        const GeoId added = round
            ? sketch.addArc(second.p[0], second.p[1], second.p[2], second.p[3], second.p[4],
                            original.construction)
            : sketch.addLine(second.p[0], second.p[1], second.p[2], second.p[3],
                             original.construction);
        result.splitInto = added;
    } else if (keepHead) {
        sketch.replaceGeometry(target, shrink(0.0, head));
    } else {
        sketch.replaceGeometry(target, shrink(tail, round ? sweep : 1.0));
    }

    dropPointConstraints();
    return result;
}

}  // namespace cad::sketch
