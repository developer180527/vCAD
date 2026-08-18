#pragma once

#include "cad/kernel/Result.h"
#include "cad/kernel/Shape.h"

#include <array>
#include <optional>
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

/// One of the three origin planes.
enum class Plane : std::uint8_t { XY, XZ, YZ };

/// WHERE a sketch is drawn — the reference, not the geometry.
///
/// A sketch on one of three global planes cannot be positioned against the model, so every feature
/// after the first has to be placed by arithmetic rather than by pointing at what is already
/// there. That is the difference between parametric modelling and drawing on three fixed planes,
/// and it is why the shell has to switch to a separate 2D canvas: the UI is faithfully
/// representing a sketch that genuinely is a separate 2D thing.
///
/// The face is held as its element name's TEXT, not as a `naming::ElementName`. `core/sketch` does
/// not depend on `core/naming` and this keeps it that way — the text form is what round-trips
/// through the saved file anyway, and resolving it to an origin and axes belongs to
/// `core/recompute`, which already has naming AND has the referenced feature's output. A sketch
/// that resolved its own face would need the document, which it must not have.
/// A sketch plane resolved into actual 3D: where its origin sits and which way its u and v axes
/// point. Produced by `core/recompute` after looking the face reference up in the element map, and
/// handed back to the sketch so `to3d` can place geometry where the user actually drew it.
///
/// Plain doubles rather than a kernel type, for the same reason the face is a string: `core/sketch`
/// does not depend on the layers that can compute this.
struct SketchFrame {
    double origin[3]{0, 0, 0};
    double u[3]{1, 0, 0};
    double v[3]{0, 1, 0};

    /// The sketch normal, u x v. Circles and arcs are swept about it.
    [[nodiscard]] std::array<double, 3> normal() const noexcept {
        return {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
    }
};

struct SketchPlane {
    enum class Kind : std::uint8_t { Global = 0, Face = 1, Datum = 2 };

    Kind kind = Kind::Global;

    /// Meaningful for Global, and kept current for the others as the fallback a viewer without
    /// face resolution can still draw on. Always serialised, which is what lets a file written by
    /// a newer build open in an older one as a plain global sketch instead of failing.
    Plane global = Plane::XY;

    /// `naming::ElementName::toString()` of the planar face. Empty unless `kind == Face`.
    std::string face;

    [[nodiscard]] friend bool operator==(const SketchPlane&, const SketchPlane&) = default;
};

enum class GeoKind : std::uint8_t { Point, Line, Circle, Arc };

/// Which characteristic point of a geometry a constraint refers to.
enum class PointRef : std::uint8_t { Start, End, Center };

using GeoId = std::uint32_t;

/// Returned by the `add*` methods when the geometry was refused. Only non-finite coordinates are
/// refused today: a NaN or an infinity accepted here reaches planegcs, whose convergence test is
/// a comparison — and every comparison with NaN is false, so it reports the system SOLVED while
/// the geometry is nonsense. From there the NaN spreads to the DXF export, the saved document and
/// any extrude built on the profile, each failing far from the cause.
///
/// Refusing at the door is the only place the value can be stopped without leaving the sketch in
/// a state its own solver cannot describe.
inline constexpr GeoId kInvalidGeo = 0xFFFFFFFEu;

/// "This constraint has no second operand" — Horizontal, Radius, LockX and the rest take one
/// piece of geometry, and this fills the unused slot.
constexpr GeoId kNoGeo = 0xFFFFFFFFu;

// Two sentinels that mean genuinely different things, so they must not BE the same thing.
// kInvalidGeo was written as static_cast<GeoId>(-1), which is 0xFFFFFFFF, which is kNoGeo — two
// names for one value on adjacent lines. Nothing was visibly broken, because the equality checks
// against each still worked; what it left behind was a refused id that is indistinguishable from
// an empty operand slot, so storing one into a constraint would have read as "no geometry here"
// rather than as an error. Held apart by an assertion so it cannot quietly close again.
static_assert(kInvalidGeo != kNoGeo,
              "a refused GeoId must not be confusable with an empty constraint operand");

/// Whether an `add*` call returned usable geometry. Prefer this to comparing against either
/// sentinel: callers should not have to know which one they might get.
[[nodiscard]] inline constexpr bool isValidGeo(GeoId id) noexcept {
    return id != kInvalidGeo && id != kNoGeo;
}

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
    explicit Sketch(Plane plane) { placement_.global = plane; }
    explicit Sketch(SketchPlane placement) : placement_(std::move(placement)) {}

    /// The global plane. Still the answer every existing caller wants: for `Kind::Global` it is
    /// exact, and for a face sketch it is the fallback until the placement is resolved (step 1b).
    [[nodiscard]] Plane plane() const noexcept { return placement_.global; }
    void setPlane(Plane p) noexcept { placement_.global = p; }

    [[nodiscard]] const SketchPlane& placement() const noexcept { return placement_; }
    void setPlacement(SketchPlane p) { placement_ = std::move(p); }

    /// True when this sketch is placed against model geometry rather than a global plane, and so
    /// cannot be turned into 3D without resolving that reference first.
    [[nodiscard]] bool needsResolution() const noexcept {
        return placement_.kind != SketchPlane::Kind::Global;
    }

    /// The placement resolved into 3D. Set by `core/recompute`, which is the layer that can look a
    /// face name up and measure it. Empty for a global-plane sketch, where the plane IS the answer.
    [[nodiscard]] const std::optional<SketchFrame>& resolvedFrame() const noexcept {
        return resolved_;
    }
    void setResolvedFrame(SketchFrame f) noexcept { resolved_ = f; }
    void clearResolvedFrame() noexcept { resolved_.reset(); }

    /// Whether this sketch can be turned into 3D geometry at all.
    ///
    /// False for a face-placed sketch whose reference has not been resolved yet. `toWire` and
    /// `toFace` REFUSE in that state rather than falling back to the global plane, because a
    /// fallback would put the profile somewhere the user never drew it and nothing downstream
    /// would know.
    [[nodiscard]] bool isPlaced() const noexcept { return !needsResolution() || resolved_.has_value(); }

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

    /// Every curve in the sketch, as a compound of edges, whatever shape they are in.
    ///
    /// The honest representation of a sketch mid-edit. `toWire` and `toFace` both REFUSE geometry
    /// that is not closed and connected — correct for building a solid, and wrong as the definition
    /// of a sketch. A sketch with one line in it is a perfectly good sketch; requiring a closed
    /// profile made drawing that line an ERROR on the feature, so the model tree showed a failure
    /// as soon as the user started work.
    ///
    /// The closed-profile requirement belongs to whatever CONSUMES the sketch. Extrude asks for a
    /// face and says so when it does not get one.
    [[nodiscard]] kernel::Result<kernel::Shape> toEdges() const;

    // ── persistence ───────────────────────────────────────────────────────────────────────

    /// Line-based text: versioned, diffable, and readable in the sqlite3 CLI. Stored as a Text
    /// property, so a sketch persists through the existing document format untouched.
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static kernel::Result<Sketch> deserialize(std::string_view);

    /// Maps a sketch-plane coordinate to 3D. Public because a viewport needs it to draw the sketch
    /// and to turn a click back into sketch coordinates.
    [[nodiscard]] std::array<double, 3> to3d(double u, double v) const;

private:
    SketchPlane placement_;
    std::optional<SketchFrame> resolved_;
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
