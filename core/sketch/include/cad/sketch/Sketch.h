#pragma once

#include "cad/kernel/Result.h"
#include "cad/kernel/Shape.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

/// A 2D sketch: geometry, constraints, and the solver that reconciles them.
///
/// This is the missing pillar of parametric CAD. Every substantial feature — extrude, revolve,
/// sweep, loft, rib — consumes a sketch, and a sketch is only useful if it is *constrained*: the
/// user states relationships ("these are perpendicular", "this is 40 mm") and the solver finds the
/// geometry that satisfies them. OCCT does not do this at all; the solver is planegcs (see
/// modules/planegcs/VENDORED.md).
///
/// Design notes that matter to callers:
///
///   * Geometry is addressed by a stable `GeoId`, never by pointer or index into a vector the
///     caller can invalidate. Constraints refer to ids, so adding geometry never disturbs them.
///   * A sketch is edited, solved, and edited again. `solve()` is cheap enough to run on every
///     drag, and it reports DEGREES OF FREEDOM — the number a user needs to know whether their
///     sketch is fully defined.
///   * Serialisation is text, so a sketch persists as an ordinary Text property in the document
///     (ADR 0003) with no schema change, and stays readable in the sqlite3 CLI.
namespace cad::sketch {

/// The plane the sketch is drawn on.
///
/// Only the three origin planes for now. Sketching on a face needs a face reference, and a
/// reference into geometry means an ElementName — which works, but ties sketches to the naming
/// layer and is a bigger change than this module needs to earn its place.
enum class Plane : std::uint8_t { XY, XZ, YZ };

enum class GeoKind : std::uint8_t { Point, Line, Circle, Arc };

/// Which characteristic point of a geometry a constraint refers to.
enum class PointRef : std::uint8_t { Start, End, Center };

using GeoId = std::uint32_t;
constexpr GeoId kNoGeo = 0xFFFFFFFFu;

/// One piece of sketch geometry.
///
/// Parameters are a flat array rather than named fields, because the solver treats them as a flat
/// vector of unknowns and every conversion between the two representations is a chance to get an
/// index wrong. What each slot means depends on `kind`:
///
///   Point   p[0..1] = x, y
///   Line    p[0..3] = x1, y1, x2, y2
///   Circle  p[0..2] = cx, cy, radius
///   Arc     p[0..4] = cx, cy, radius, startAngle, endAngle   (radians, CCW)
struct Geometry {
    GeoKind kind = GeoKind::Point;
    /// Construction geometry guides other geometry but is not part of the profile — excluded from
    /// `toWire`. Sketches routinely depend on centrelines that must not become edges of the solid.
    bool construction = false;
    std::array<double, 5> p{};
};

enum class ConstraintKind : std::uint8_t {
    Coincident,     ///< two points occupy the same place
    Horizontal,     ///< a line is horizontal
    Vertical,       ///< a line is vertical
    Parallel,       ///< two lines
    Perpendicular,  ///< two lines
    Distance,       ///< between two points, `value` apart
    Radius,         ///< a circle or arc has `value` radius
    PointOnLine,    ///< a point lies somewhere on a line
    EqualLength,    ///< two lines, same length
    LockX,          ///< a point's x is fixed at `value`
    LockY,          ///< a point's y is fixed at `value`
};

struct Constraint {
    ConstraintKind kind = ConstraintKind::Coincident;
    GeoId a = kNoGeo;
    PointRef aPoint = PointRef::Start;
    GeoId b = kNoGeo;
    PointRef bPoint = PointRef::Start;
    double value = 0.0;
};

/// What the solver made of the sketch.
struct SolveReport {
    bool solved = false;
    /// Remaining degrees of freedom. 0 means fully constrained — the state a production sketch
    /// should reach. Negative means over-constrained.
    int dofs = 0;
    /// Indices into `constraints()` that cannot all hold at once. This is the difference between a
    /// solver a user can work with and one that just fails: it names the constraints to remove.
    std::vector<std::size_t> conflicting;
    /// Constraints that are satisfied but add nothing. Harmless, worth telling the user about.
    std::vector<std::size_t> redundant;
    std::string message;
};

class Sketch {
public:
    Sketch() = default;
    explicit Sketch(Plane plane) : plane_(plane) {}

    [[nodiscard]] Plane plane() const noexcept { return plane_; }
    void setPlane(Plane p) noexcept { plane_ = p; }

    // ── geometry ──────────────────────────────────────────────────────────────────────────
    GeoId addPoint(double x, double y, bool construction = false);
    GeoId addLine(double x1, double y1, double x2, double y2, bool construction = false);
    GeoId addCircle(double cx, double cy, double radius, bool construction = false);
    GeoId addArc(double cx, double cy, double radius, double startAngle, double endAngle,
                 bool construction = false);

    [[nodiscard]] const std::vector<Geometry>& geometry() const noexcept { return geometry_; }
    /// Ids, index-aligned with geometry(). Exposed so callers that walk geometry positionally can
    /// name what they found — constraint inference needs exactly this, and reconstructing it by
    /// probing find() was both fragile and O(n^2).
    [[nodiscard]] const std::vector<GeoId>& ids() const noexcept { return ids_; }
    [[nodiscard]] const Geometry* find(GeoId) const noexcept;

    /// Reads a characteristic point in sketch coordinates. Fails for a point a geometry does not
    /// have — a Circle has a Center but no Start.
    [[nodiscard]] kernel::Result<std::array<double, 2>> pointAt(GeoId, PointRef) const;

    // ── constraints ───────────────────────────────────────────────────────────────────────
    //
    // Each returns the constraint's index, which is what SolveReport reports conflicts by.
    std::size_t coincident(GeoId a, PointRef ap, GeoId b, PointRef bp);
    std::size_t horizontal(GeoId line);
    std::size_t vertical(GeoId line);
    std::size_t parallel(GeoId l1, GeoId l2);
    std::size_t perpendicular(GeoId l1, GeoId l2);
    std::size_t distance(GeoId a, PointRef ap, GeoId b, PointRef bp, double value);
    std::size_t radius(GeoId circleOrArc, double value);
    std::size_t pointOnLine(GeoId point, PointRef pp, GeoId line);
    std::size_t equalLength(GeoId l1, GeoId l2);
    std::size_t lockX(GeoId g, PointRef pp, double value);
    std::size_t lockY(GeoId g, PointRef pp, double value);

    [[nodiscard]] const std::vector<Constraint>& constraints() const noexcept {
        return constraints_;
    }
    void removeConstraint(std::size_t index);

    // ── solving ───────────────────────────────────────────────────────────────────────────

    /// Solves in place: on success the geometry holds the solved positions.
    ///
    /// Runs the solver's diagnosis first. That is not optional — planegcs's conflict and
    /// redundancy report is only populated by `diagnose()`, and `solve()` alone leaves it stale, so
    /// skipping it means reporting "solved" for a sketch the solver knows is contradictory.
    SolveReport solve();

    // ── geometry out ──────────────────────────────────────────────────────────────────────

    /// The profile as a single closed OCCT wire, in 3D on the sketch's plane.
    ///
    /// Construction geometry and standalone points are excluded. Fails if the curves do not form
    /// exactly one closed loop — an open profile cannot be extruded into a solid, and saying so
    /// here is far better than handing OCCT something that fails deep inside a modelling operation.
    [[nodiscard]] kernel::Result<kernel::Shape> toWire() const;

    /// The profile as a planar face, which is what an extrude consumes.
    [[nodiscard]] kernel::Result<kernel::Shape> toFace() const;

    // ── persistence ───────────────────────────────────────────────────────────────────────

    /// Line-based text: versioned, diffable, and readable in the sqlite3 CLI. Stored as a Text
    /// property, so a sketch persists through the existing document format untouched.
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static kernel::Result<Sketch> deserialize(std::string_view);

    /// Maps a sketch-plane coordinate to 3D. Public because a viewport needs it to draw the sketch
    /// and to turn a click back into sketch coordinates.
    [[nodiscard]] std::array<double, 3> to3d(double u, double v) const;

private:
    Plane plane_ = Plane::XY;
    std::vector<Geometry> geometry_;
    std::vector<Constraint> constraints_;
    /// Parallel to `geometry_`, holding each entry's id. Ids are handed out monotonically and never
    /// reused, so deleting geometry cannot silently re-point a constraint at something else.
    std::vector<GeoId> ids_;
    GeoId nextId_ = 0;
};

[[nodiscard]] const char* toString(Plane) noexcept;
[[nodiscard]] const char* toString(GeoKind) noexcept;
[[nodiscard]] const char* toString(ConstraintKind) noexcept;

}  // namespace cad::sketch
