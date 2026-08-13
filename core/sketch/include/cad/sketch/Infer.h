#pragma once

#include "cad/sketch/Sketch.h"

#include <cstddef>
#include <string>

/// Constraint inference: turning dumb geometry into a parametric sketch.
///
/// An imported DXF is coordinates and nothing else. Every line floats independently, so moving one
/// edge of a profile leaves the others behind — which defeats the entire point of a sketch. This
/// looks at geometry that is *nearly* related and asserts that it *is*: endpoints that almost touch
/// are coincident, lines that are almost axis-aligned are horizontal or vertical.
///
/// The hard part is not finding those relationships. It is **not adding too many**.
///
/// A solver told the same fact twice reports redundancy; told two facts that cannot both hold, it
/// reports a conflict. Either makes a freshly imported sketch look broken. So the rules below are
/// as much about what NOT to emit as what to emit:
///
///   * A cluster of k coincident endpoints gets k-1 constraints, not k(k-1)/2. Chaining them is
///     sufficient; all-pairs is the same fact repeated and floods the redundancy report.
///   * Parallel and perpendicular are only inferred between lines that are NOT already horizontal
///     or vertical. Two horizontal lines are already parallel; saying so again is redundant.
///
/// Tolerances are the user's business, not ours. Too loose fuses points that were meant to stay
/// apart, and there is no way to detect that automatically — the geometry looks identical either
/// way. So they are explicit, defaulted conservatively, and the UI should expose them.
namespace cad::sketch {

struct InferenceOptions {
    /// Endpoints closer than this are treated as the same point. Document units (mm).
    ///
    /// Conservative on purpose. CAD exports routinely leave gaps of ~1e-6 at corners that were
    /// meant to meet; they rarely leave two distinct features 0.01 apart.
    double pointTolerance = 0.01;

    /// A line within this many degrees of an axis is called horizontal or vertical.
    ///
    /// Also conservative: a 2-degree taper is a design intent, not a drafting error, and asserting
    /// it is horizontal silently destroys the part.
    double angleToleranceDeg = 0.5;

    bool coincident = true;
    bool horizontalVertical = true;

    /// Off by default. Parallel and perpendicular are much more likely to be coincidental than
    /// intended — two arbitrary lines in a real drawing are perpendicular surprisingly often — and a
    /// wrongly inferred one is far harder for a user to find than a missing one.
    bool parallelPerpendicular = false;
};

struct InferenceReport {
    std::size_t coincident = 0;
    std::size_t horizontal = 0;
    std::size_t vertical = 0;
    std::size_t parallel = 0;
    std::size_t perpendicular = 0;

    /// Degrees of freedom before and after. This is the number that says whether inference
    /// accomplished anything: 40 -> 4 means the profile became a shape with a position, which is
    /// what a user wants to see.
    int dofsBefore = 0;
    int dofsAfter = 0;

    /// Non-empty means inference produced a contradiction — a bug in the rules, or a tolerance so
    /// loose that it fused geometry that genuinely differed. Reported rather than swallowed.
    std::size_t conflicting = 0;
    std::size_t redundant = 0;

    [[nodiscard]] std::size_t added() const noexcept {
        return coincident + horizontal + vertical + parallel + perpendicular;
    }
    [[nodiscard]] std::string summary() const;
};

/// Adds inferred constraints to `sketch` in place, then solves it.
///
/// Solving is part of the job rather than left to the caller: inference is only meaningful if the
/// result still solves, and the DOF figures in the report come from that solve.
InferenceReport infer(Sketch& sketch, const InferenceOptions& options = {});

}  // namespace cad::sketch
