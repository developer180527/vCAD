#pragma once

// Trimming: cut a curve back to where it meets its neighbours.
//
// The single most-reached-for sketch edit, and the one whose absence forces every profile to be
// drawn exactly right the first time. Draw past a corner, click the overhang, and it is gone.
//
// Here rather than in `app/` because it is arithmetic over a `Sketch` and nothing else — no
// toolkit, no kernel, no camera. Both shells get it from one implementation, and so does the plugin
// ABI when it wants it.

#include "cad/kernel/Result.h"
#include "cad/sketch/Sketch.h"

#include <array>
#include <cstddef>
#include <vector>

namespace cad::sketch {

/// What a trim did, so a shell can say it.
struct TrimResult {
    /// The piece the click removed no longer exists at all — the curve had nothing cutting it, so
    /// there was no span to cut back TO and the whole curve went.
    bool removedWhole = false;

    /// The trim split the curve in two: the id given back is the new second piece. The original id
    /// still names the first piece, which is what keeps its constraints attached.
    std::optional<GeoId> splitInto;

    /// Constraints dropped because the trim moved a point they pinned.
    std::size_t constraintsDropped = 0;
};

/// Cuts the span of `target` containing `at` back to its nearest intersections.
///
/// `at` is in sketch coordinates — the point the user clicked, which selects WHICH span of the
/// curve to remove. That is the whole interaction: a curve crossed twice has three spans, and only
/// the click says which one the user meant.
///
/// The rules, chosen to match what every CAD sketcher does:
///
/// - No intersections at all: the whole curve is removed. There is nothing to cut back to, and
///   refusing would make the tool useless exactly where a stray line most needs deleting.
/// - A span at either end: the curve is SHORTENED, keeping its id and its constraints.
/// - A span in the middle: the curve is split in two, the original id keeping the first piece.
/// - A circle: needs two intersections to become an arc; with fewer, the whole circle goes.
///
/// Constraints that name a POINT of the trimmed curve are dropped — coincidence, distance,
/// point-on-line, the locks. A trim moves endpoints by definition, so a constraint pinning one that
/// just moved would either drag the geometry back on the next solve or make the sketch
/// over-constrained, and both look like the trim failing. Constraints that describe the curve's
/// SHAPE rather than its ends survive: horizontal, vertical, parallel, perpendicular, equal length,
/// radius.
///
/// Fails only when the id is not in the sketch, or when the geometry is a point — a point has no
/// span to trim, and saying so beats silently doing nothing.
[[nodiscard]] kernel::Result<TrimResult> trim(Sketch&, GeoId target, std::array<double, 2> at);

/// Every point at which two sketch geometries cross, in sketch coordinates.
///
/// Exposed because it is independently useful — snapping, inference and a future Extend all need
/// it — and because trimming is far easier to test when the intersection maths can be checked on
/// its own. Points are filtered to lie WITHIN both curves: a line's segment bounds and an arc's
/// angular range both count, so two curves whose infinite extensions cross do not intersect here.
[[nodiscard]] std::vector<std::array<double, 2>> intersections(const Geometry& a,
                                                               const Geometry& b);

}  // namespace cad::sketch
