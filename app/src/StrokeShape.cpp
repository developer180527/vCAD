#include "cad/app/StrokeShape.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace cad::app {

namespace {

/// A stroke must bend by at least this fraction of its own length to count as an arc.
///
/// The tolerance alone is not enough. A long stroke drawn with a shaky hand accumulates a few
/// millimetres of wander over hundreds, and fitting an arc to that produces a circle the size of a
/// building — geometrically a fine answer and never what anyone meant. Requiring the bend to be
/// proportional to the stroke's length is what distinguishes "curved" from "long and imperfect".
///
/// 2% of the chord is a sagitta of 2mm over 100mm, which is a visible curve and about the smallest
/// one a person draws deliberately.
constexpr double kMinSagittaRatio = 0.02;

/// An arc has to actually turn. Below this it is a line drawn by a hand.
///
/// The rule the other two could not express. A stroke's straightness was judged only against a
/// pixel tolerance and a fraction of its own length, and neither protects a SHORT stroke: five
/// pixels of stylus wobble over fifty is more than four pixels and more than 2% of the chord, so it
/// fitted an arc — of enormous radius, drawn as a wild curve across the sketch. Reported as "random
/// curves and circles get sketched when I try to draw a line", and it is the single worst thing a
/// sketcher can do, because the user has to notice and delete each one.
///
/// Twenty degrees is well below any arc a person draws deliberately — a quarter circle is ninety —
/// and well above what a hand produces by accident.
constexpr double kMinSweptRadians = 20.0 * std::numbers::pi / 180.0;

double distanceBetween(const StrokePoint& a, const StrokePoint& b) {
    const double dx = b[0] - a[0];
    const double dy = b[1] - a[1];
    return std::sqrt(dx * dx + dy * dy);
}

}   // namespace

StrokeFit fitStroke(std::span<const StrokePoint> points, double tolerance) {
    StrokeFit fit;
    if (points.size() < 2) return fit;

    fit.start = points.front();
    fit.end = points.back();

    const double chordX = fit.end[0] - fit.start[0];
    const double chordY = fit.end[1] - fit.start[1];
    const double chord = std::sqrt(chordX * chordX + chordY * chordY);

    // A stroke that ends where it began has no chord to measure against, so neither test below
    // means anything. Reported as nothing rather than guessed at: a closed loop drawn in one
    // gesture is a circle, and inventing one here from a shape this function cannot verify would be
    // the "guesses wrong 5% of the time" failure the header warns about.
    if (chord < 1e-9) return fit;

    // Signed perpendicular distance from the chord, keeping BOTH extremes.
    //
    // Both, because their signs are the whole S-curve test below. Taking the magnitude alone loses
    // exactly the information that says a stroke bent one way and then the other.
    const double nx = -chordY / chord;   // chord normal, unit length
    const double ny = chordX / chord;
    double most = 0.0;
    double least = 0.0;
    std::size_t apex = 0;
    for (std::size_t i = 1; i + 1 < points.size(); ++i) {
        const double d = (points[i][0] - fit.start[0]) * nx + (points[i][1] - fit.start[1]) * ny;
        if (d > most) most = d;
        if (d < least) least = d;
        if (std::abs(d) > std::abs((points[apex][0] - fit.start[0]) * nx
                                   + (points[apex][1] - fit.start[1]) * ny)) {
            apex = i;
        }
    }
    fit.deviation = std::max(most, -least);

    // Straight enough, or not bent enough for its length.
    if (fit.deviation <= tolerance || fit.deviation < chord * kMinSagittaRatio) {
        fit.kind = StrokeKind::Line;
        return fit;
    }

    // An S: the stroke bent materially to BOTH sides of its chord. No single arc passes through
    // that, and the honest answers are a spline or a line. A line, for now — because the caller can
    // chain another stroke onto its endpoint and get the shape they wanted, whereas a wrong arc has
    // to be found and deleted. Splines are a separate tool, not a silent fallback.
    if (most > tolerance && -least > tolerance) {
        fit.kind = StrokeKind::Line;
        return fit;
    }

    // Circle through start, apex, end.
    //
    // Three points rather than a least-squares fit over all of them. Least squares is more accurate
    // for a clean stroke and WORSE for a real one: it lets the dense cluster of samples where the
    // hand slowed down at the end of the stroke drag the centre, so the arc misses the endpoint the
    // user actually finished on. The endpoints are the part that must be exact, because the next
    // stroke chains onto one of them.
    const StrokePoint& a = fit.start;
    const StrokePoint& b = points[apex];
    const StrokePoint& c = fit.end;
    const double area = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
    if (std::abs(area) < 1e-12) {
        fit.kind = StrokeKind::Line;   // collinear after all
        return fit;
    }

    const double aa = a[0] * a[0] + a[1] * a[1];
    const double bb = b[0] * b[0] + b[1] * b[1];
    const double cc = c[0] * c[0] + c[1] * c[1];
    const double ux = (aa * (b[1] - c[1]) + bb * (c[1] - a[1]) + cc * (a[1] - b[1])) / (2.0 * area);
    const double uy = (aa * (c[0] - b[0]) + bb * (a[0] - c[0]) + cc * (b[0] - a[0])) / (2.0 * area);

    fit.centre = StrokePoint{ux, uy};
    fit.radius = distanceBetween(fit.centre, a);
    if (!std::isfinite(fit.radius) || fit.radius < 1e-9) {
        fit.kind = StrokeKind::Line;
        return fit;
    }

    double from = std::atan2(a[1] - uy, a[0] - ux);
    double to = std::atan2(c[1] - uy, c[0] - ux);

    // `Sketch::addArc` sweeps COUNTER-CLOCKWISE from start to end. `area` is twice the signed area
    // of the triangle, so its sign is the stroke's direction: negative means the hand went
    // clockwise, and the same arc is expressed by swapping the ends.
    //
    // Swapped here rather than reported as a flag, so a caller cannot forget to apply it — which
    // would draw the complement of the arc: the long way round, through the wrong side of the
    // circle, which is a spectacular and entirely silent failure.
    if (area < 0.0) std::swap(from, to);

    // HOW FAR IT TURNS, which is what separates an arc from a wobble. Measured after the fit
    // because it needs the centre: a nearly straight stroke fits a circle whose radius is enormous
    // and whose swept angle is a degree or two.
    double swept = to - from;
    while (swept <= 0.0) swept += 2.0 * std::numbers::pi;
    while (swept > 2.0 * std::numbers::pi) swept -= 2.0 * std::numbers::pi;
    // The short way round: `from` and `to` were ordered for a counter-clockwise sweep, so a
    // near-straight clockwise stroke reads as almost a full turn rather than almost none.
    const double turn = std::min(swept, 2.0 * std::numbers::pi - swept);
    if (turn < kMinSweptRadians) {
        fit.kind = StrokeKind::Line;
        return fit;
    }

    fit.kind = StrokeKind::Arc;
    fit.startAngle = from;
    fit.endAngle = to;
    return fit;
}

}   // namespace cad::app
