#pragma once

/// What a hand-drawn stroke was meant to be.
///
/// # The one decision this makes
///
/// Shapr3D's signature sketching gesture is a single tool that "automatically switches your sketch
/// to a line or arc depending on your pen gesture" — pen down, drag, lift, and one primitive comes
/// out. This is that decision, and nothing else: given the points a pointer travelled through,
/// was the user drawing a straight line or an arc, and if an arc, which one.
///
/// It is deliberately NOT freehand shape recognition. Nobody scribbles a rough rectangle and gets
/// four constrained lines — each stroke is one primitive. That is what makes the problem tractable
/// and the result predictable, which matters more here than cleverness: a sketcher that guesses
/// wrong 5% of the time is worse than one that never guesses, because every stroke must then be
/// checked.
///
/// # Why it is a free function over plain points
///
/// No sketch, no document, no camera, no units beyond "the caller's own". The interesting cases —
/// a wobbly straight line, a gentle arc, an S that is neither — are then writable as a dozen
/// coordinates in a test, which is the only way this gets tuned honestly. Everything about WHERE
/// the stroke came from belongs to the caller.
///
/// See docs/design/SKETCHING_IPAD.md for the research this implements.

#include <array>
#include <cstdint>
#include <span>

namespace cad::app {

/// A point in the sketch's own 2D coordinates, in millimetres. Same convention as `SketchDrawing`.
using StrokePoint = std::array<double, 2>;

enum class StrokeKind : std::uint8_t {
    /// Fewer than two distinguishable points: a tap, not a stroke.
    Nothing,
    Line,
    Arc,
};

struct StrokeFit {
    StrokeKind kind = StrokeKind::Nothing;

    StrokePoint start{};
    StrokePoint end{};

    // Arc only.
    StrokePoint centre{};
    double radius = 0.0;
    /// Radians, measured from the sketch's +u axis. `Sketch::addArc` sweeps counter-clockwise from
    /// `startAngle` to `endAngle`, so these are already ordered for it — a clockwise stroke comes
    /// back with its ends swapped rather than with a flag the caller has to remember.
    double startAngle = 0.0;
    double endAngle = 0.0;

    /// How far the stroke departed from the straight line between its ends, at its worst, in
    /// millimetres. The number the decision was made on; returned so a caller can say why.
    double deviation = 0.0;
};

/// Fits `points` to a line or an arc.
///
/// `tolerance` is how far the stroke may wander from straight and still be a line, in the same
/// units as the points. It is a HAND tolerance, not a geometric one: the caller derives it from
/// pixels (see `SketchDrawing`), because a wobble smaller than the hand's own precision means
/// nothing at any zoom level, whereas a fixed millimetre tolerance calls everything an arc when
/// zoomed out and everything a line when zoomed in.
[[nodiscard]] StrokeFit fitStroke(std::span<const StrokePoint> points, double tolerance);

}   // namespace cad::app
