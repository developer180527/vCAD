#include "cad/sketch/Sketch.h"

#include "cad/kernel/Guard.h"
#include "cad/kernel/internal/Occt.h"

#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <GC_MakeArcOfCircle.hxx>
#include <gp_Ax2.hxx>
#include <gp_Circ.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Wire.hxx>

// planegcs. Included here and nowhere else: the solver is an implementation detail of this module,
// and GCS.h drags in Eigen and Boost.Graph, which nothing above core/sketch should pay for.
#include "GCS.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <sstream>
#include <string>

namespace cad::sketch {
namespace {

using kernel::Error;
using kernel::ErrorCode;

constexpr int kParamsFor[]{2, 4, 3, 5};   // Point, Line, Circle, Arc — indexed by GeoKind

std::size_t paramCount(GeoKind kind) {
    return static_cast<std::size_t>(kParamsFor[static_cast<int>(kind)]);
}

}  // namespace

const char* toString(Plane p) noexcept {
    switch (p) {
        case Plane::XY: return "XY";
        case Plane::XZ: return "XZ";
        case Plane::YZ: return "YZ";
    }
    return "XY";
}

const char* toString(GeoKind k) noexcept {
    switch (k) {
        case GeoKind::Point:  return "point";
        case GeoKind::Line:   return "line";
        case GeoKind::Circle: return "circle";
        case GeoKind::Arc:    return "arc";
    }
    return "point";
}

const char* toString(ConstraintKind k) noexcept {
    switch (k) {
        case ConstraintKind::Coincident:    return "coincident";
        case ConstraintKind::Horizontal:    return "horizontal";
        case ConstraintKind::Vertical:      return "vertical";
        case ConstraintKind::Parallel:      return "parallel";
        case ConstraintKind::Perpendicular: return "perpendicular";
        case ConstraintKind::Distance:      return "distance";
        case ConstraintKind::Radius:        return "radius";
        case ConstraintKind::PointOnLine:   return "pointonline";
        case ConstraintKind::EqualLength:   return "equallength";
        case ConstraintKind::LockX:         return "lockx";
        case ConstraintKind::LockY:         return "locky";
    }
    return "coincident";
}

// ── geometry ────────────────────────────────────────────────────────────────────────────

GeoId Sketch::addPoint(double x, double y, bool construction) {
    // The same guard its three siblings carry. A point is the one piece of geometry that can be
    // referenced by a coincidence without ever being drawn, so a non-finite one propagates into
    // the solver just as readily as a line does.
    if (!std::isfinite(x) || !std::isfinite(y)) return kInvalidGeo;
    Geometry g;
    g.kind = GeoKind::Point;
    g.construction = construction;
    g.p[0] = x;
    g.p[1] = y;
    geometry_.push_back(g);
    ids_.push_back(nextId_);
    return nextId_++;
}

GeoId Sketch::addLine(double x1, double y1, double x2, double y2, bool construction) {
    if (!std::isfinite(x1) || !std::isfinite(y1) || !std::isfinite(x2) || !std::isfinite(y2)) {
        return kInvalidGeo;
    }
    Geometry g;
    g.kind = GeoKind::Line;
    g.construction = construction;
    g.p = {x1, y1, x2, y2, 0.0};
    geometry_.push_back(g);
    ids_.push_back(nextId_);
    return nextId_++;
}

GeoId Sketch::addCircle(double cx, double cy, double radius, bool construction) {
    if (!std::isfinite(cx) || !std::isfinite(cy) || !std::isfinite(radius)) {
        return kInvalidGeo;
    }
    Geometry g;
    g.kind = GeoKind::Circle;
    g.construction = construction;
    g.p = {cx, cy, radius, 0.0, 0.0};
    geometry_.push_back(g);
    ids_.push_back(nextId_);
    return nextId_++;
}

GeoId Sketch::addArc(double cx, double cy, double radius, double startAngle, double endAngle,
                     bool construction) {
    if (!std::isfinite(cx) || !std::isfinite(cy) || !std::isfinite(radius)
        || !std::isfinite(startAngle) || !std::isfinite(endAngle)) {
        return kInvalidGeo;
    }
    Geometry g;
    g.kind = GeoKind::Arc;
    g.construction = construction;
    g.p = {cx, cy, radius, startAngle, endAngle};
    geometry_.push_back(g);
    ids_.push_back(nextId_);
    return nextId_++;
}

const Geometry* Sketch::find(GeoId id) const noexcept {
    for (std::size_t i = 0; i < ids_.size(); ++i) {
        if (ids_[i] == id) return &geometry_[i];
    }
    return nullptr;
}

kernel::Result<std::array<double, 2>> Sketch::pointAt(GeoId id, PointRef ref) const {
    const Geometry* g = find(id);
    if (g == nullptr) return Error{ErrorCode::InvalidInput, "That sketch geometry does not exist."};

    switch (g->kind) {
        case GeoKind::Point:
            if (ref != PointRef::Start && ref != PointRef::Center) break;
            return std::array<double, 2>{g->p[0], g->p[1]};
        case GeoKind::Line:
            if (ref == PointRef::Start) return std::array<double, 2>{g->p[0], g->p[1]};
            if (ref == PointRef::End) return std::array<double, 2>{g->p[2], g->p[3]};
            break;
        case GeoKind::Circle:
            if (ref == PointRef::Center) return std::array<double, 2>{g->p[0], g->p[1]};
            break;
        case GeoKind::Arc: {
            if (ref == PointRef::Center) return std::array<double, 2>{g->p[0], g->p[1]};
            const double angle = ref == PointRef::Start ? g->p[3] : g->p[4];
            return std::array<double, 2>{g->p[0] + g->p[2] * std::cos(angle),
                                         g->p[1] + g->p[2] * std::sin(angle)};
        }
    }
    return Error{ErrorCode::InvalidInput,
                 std::string("A ") + toString(g->kind) + " has no such point."};
}

// ── constraints ─────────────────────────────────────────────────────────────────────────

namespace {
std::size_t push(std::vector<Constraint>& out, Constraint c) {
    out.push_back(c);
    return out.size() - 1;
}
}  // namespace

std::size_t Sketch::coincident(GeoId a, PointRef ap, GeoId b, PointRef bp) {
    return push(constraints_, {ConstraintKind::Coincident, a, ap, b, bp, 0.0});
}
std::size_t Sketch::horizontal(GeoId line) {
    return push(constraints_,
                {ConstraintKind::Horizontal, line, PointRef::Start, kNoGeo, PointRef::Start, 0.0});
}
std::size_t Sketch::vertical(GeoId line) {
    return push(constraints_,
                {ConstraintKind::Vertical, line, PointRef::Start, kNoGeo, PointRef::Start, 0.0});
}
std::size_t Sketch::parallel(GeoId l1, GeoId l2) {
    return push(constraints_,
                {ConstraintKind::Parallel, l1, PointRef::Start, l2, PointRef::Start, 0.0});
}
std::size_t Sketch::perpendicular(GeoId l1, GeoId l2) {
    return push(constraints_,
                {ConstraintKind::Perpendicular, l1, PointRef::Start, l2, PointRef::Start, 0.0});
}
std::size_t Sketch::distance(GeoId a, PointRef ap, GeoId b, PointRef bp, double value) {
    return push(constraints_, {ConstraintKind::Distance, a, ap, b, bp, value});
}
std::size_t Sketch::radius(GeoId circleOrArc, double value) {
    return push(constraints_, {ConstraintKind::Radius, circleOrArc, PointRef::Center, kNoGeo,
                               PointRef::Start, value});
}
std::size_t Sketch::pointOnLine(GeoId point, PointRef pp, GeoId line) {
    return push(constraints_, {ConstraintKind::PointOnLine, point, pp, line, PointRef::Start, 0.0});
}
std::size_t Sketch::equalLength(GeoId l1, GeoId l2) {
    return push(constraints_,
                {ConstraintKind::EqualLength, l1, PointRef::Start, l2, PointRef::Start, 0.0});
}
std::size_t Sketch::lockX(GeoId g, PointRef pp, double value) {
    return push(constraints_, {ConstraintKind::LockX, g, pp, kNoGeo, PointRef::Start, value});
}
std::size_t Sketch::lockY(GeoId g, PointRef pp, double value) {
    return push(constraints_, {ConstraintKind::LockY, g, pp, kNoGeo, PointRef::Start, value});
}

void Sketch::removeConstraint(std::size_t index) {
    if (index < constraints_.size()) {
        constraints_.erase(constraints_.begin() + static_cast<std::ptrdiff_t>(index));
    }
}

// ── solving ─────────────────────────────────────────────────────────────────────────────

SolveReport Sketch::solve() {
    SolveReport report;
    // Kept so a solve that produces nonsense can put the sketch back exactly as it was. See the
    // finiteness check at the end.
    const std::vector<Geometry> before = geometry_;
    if (geometry_.empty()) {
        report.solved = true;
        report.message = "Empty sketch.";
        return report;
    }

    // The solver mutates doubles through raw pointers it keeps for the whole solve, so the
    // container holding them MUST NOT reallocate. A std::vector that grows would leave GCS writing
    // into freed memory — a crash or silent corruption with nothing pointing at the cause. A deque
    // never moves an existing element.
    std::deque<double> params;
    std::vector<double*> unknowns;

    // GCS geometry objects hold pointers into `params`, so these also have to outlive the solve and
    // keep stable addresses. Held by deque for the same reason.
    std::deque<GCS::Point> points;
    std::deque<GCS::Line> lines;
    std::deque<GCS::Circle> circles;
    std::deque<GCS::Arc> arcs;

    // Per geometry entry, where its GCS object lives. Indices, not pointers, because the deques are
    // still being appended to while this is built.
    struct Mapping {
        GeoKind kind = GeoKind::Point;
        std::size_t index = 0;      ///< into points/lines/circles/arcs
    };
    std::vector<Mapping> mapping(geometry_.size());

    const auto addParam = [&](double value) {
        params.push_back(value);
        double* p = &params.back();
        unknowns.push_back(p);
        return p;
    };

    for (std::size_t i = 0; i < geometry_.size(); ++i) {
        Geometry& g = geometry_[i];
        switch (g.kind) {
            case GeoKind::Point: {
                GCS::Point p;
                p.x = addParam(g.p[0]);
                p.y = addParam(g.p[1]);
                points.push_back(p);
                mapping[i] = {GeoKind::Point, points.size() - 1};
                break;
            }
            case GeoKind::Line: {
                GCS::Line l;
                l.p1.x = addParam(g.p[0]);
                l.p1.y = addParam(g.p[1]);
                l.p2.x = addParam(g.p[2]);
                l.p2.y = addParam(g.p[3]);
                lines.push_back(l);
                mapping[i] = {GeoKind::Line, lines.size() - 1};
                break;
            }
            case GeoKind::Circle: {
                GCS::Circle c;
                c.center.x = addParam(g.p[0]);
                c.center.y = addParam(g.p[1]);
                c.rad = addParam(g.p[2]);
                circles.push_back(c);
                mapping[i] = {GeoKind::Circle, circles.size() - 1};
                break;
            }
            case GeoKind::Arc: {
                GCS::Arc a;
                a.center.x = addParam(g.p[0]);
                a.center.y = addParam(g.p[1]);
                a.rad = addParam(g.p[2]);
                a.startAngle = addParam(g.p[3]);
                a.endAngle = addParam(g.p[4]);
                // Arc endpoints are parameters the solver maintains, tied to centre/radius/angles
                // by an ArcRules constraint. Without that constraint the endpoints drift free of
                // the arc they belong to and every constraint attached to them is meaningless.
                a.start.x = addParam(g.p[0] + g.p[2] * std::cos(g.p[3]));
                a.start.y = addParam(g.p[1] + g.p[2] * std::sin(g.p[3]));
                a.end.x = addParam(g.p[0] + g.p[2] * std::cos(g.p[4]));
                a.end.y = addParam(g.p[1] + g.p[2] * std::sin(g.p[4]));
                arcs.push_back(a);
                mapping[i] = {GeoKind::Arc, arcs.size() - 1};
                break;
            }
        }
    }

    GCS::System sys;
    for (std::size_t i = 0; i < arcs.size(); ++i) sys.addConstraintArcRules(arcs[i]);

    // Resolves (geometry, point) to the GCS::Point the solver knows about — which is the one inside
    // the GCS object, not a copy. Returning a pointer into the deques is safe: they are complete
    // and never appended to after this point.
    const auto gcsPoint = [&](GeoId id, PointRef ref) -> GCS::Point* {
        for (std::size_t i = 0; i < ids_.size(); ++i) {
            if (ids_[i] != id) continue;
            const Mapping& m = mapping[i];
            switch (m.kind) {
                case GeoKind::Point:  return &points[m.index];
                case GeoKind::Line:
                    return ref == PointRef::End ? &lines[m.index].p2 : &lines[m.index].p1;
                case GeoKind::Circle: return &circles[m.index].center;
                case GeoKind::Arc:
                    if (ref == PointRef::Center) return &arcs[m.index].center;
                    return ref == PointRef::End ? &arcs[m.index].end : &arcs[m.index].start;
            }
        }
        return nullptr;
    };
    const auto gcsLine = [&](GeoId id) -> GCS::Line* {
        for (std::size_t i = 0; i < ids_.size(); ++i) {
            if (ids_[i] == id && mapping[i].kind == GeoKind::Line) return &lines[mapping[i].index];
        }
        return nullptr;
    };
    const auto gcsRadius = [&](GeoId id) -> GCS::Circle* {
        for (std::size_t i = 0; i < ids_.size(); ++i) {
            if (ids_[i] != id) continue;
            if (mapping[i].kind == GeoKind::Circle) return &circles[mapping[i].index];
            if (mapping[i].kind == GeoKind::Arc) return &arcs[mapping[i].index];
        }
        return nullptr;
    };

    // Constraint VALUES are parameters too, but NOT unknowns: the solver must satisfy "40 mm", not
    // adjust it. Kept in their own container so they are never declared as unknowns by accident,
    // which would let the solver "solve" a sketch by changing the dimensions the user typed.
    std::deque<double> values;
    const auto value = [&](double v) {
        values.push_back(v);
        return &values.back();
    };

    // Tags are 1-based and index constraints_: the solver reports conflicts BY TAG, and tag 0 means
    // "untagged" in planegcs, so a 0-based tag would make the first constraint unreportable.
    for (std::size_t ci = 0; ci < constraints_.size(); ++ci) {
        const Constraint& c = constraints_[ci];
        const int tag = static_cast<int>(ci) + 1;
        switch (c.kind) {
            case ConstraintKind::Coincident: {
                auto* a = gcsPoint(c.a, c.aPoint);
                auto* b = gcsPoint(c.b, c.bPoint);
                if (a != nullptr && b != nullptr) sys.addConstraintP2PCoincident(*a, *b, tag);
                break;
            }
            case ConstraintKind::Horizontal:
                if (auto* l = gcsLine(c.a)) sys.addConstraintHorizontal(*l, tag);
                break;
            case ConstraintKind::Vertical:
                if (auto* l = gcsLine(c.a)) sys.addConstraintVertical(*l, tag);
                break;
            case ConstraintKind::Parallel: {
                auto* l1 = gcsLine(c.a);
                auto* l2 = gcsLine(c.b);
                if (l1 != nullptr && l2 != nullptr) sys.addConstraintParallel(*l1, *l2, tag);
                break;
            }
            case ConstraintKind::Perpendicular: {
                auto* l1 = gcsLine(c.a);
                auto* l2 = gcsLine(c.b);
                if (l1 != nullptr && l2 != nullptr) sys.addConstraintPerpendicular(*l1, *l2, tag);
                break;
            }
            case ConstraintKind::Distance: {
                auto* a = gcsPoint(c.a, c.aPoint);
                auto* b = gcsPoint(c.b, c.bPoint);
                if (a != nullptr && b != nullptr) {
                    sys.addConstraintP2PDistance(*a, *b, value(c.value), tag);
                }
                break;
            }
            case ConstraintKind::Radius:
                if (auto* circle = gcsRadius(c.a)) {
                    sys.addConstraintCircleRadius(*circle, value(c.value), tag);
                }
                break;
            case ConstraintKind::PointOnLine: {
                auto* p = gcsPoint(c.a, c.aPoint);
                auto* l = gcsLine(c.b);
                if (p != nullptr && l != nullptr) sys.addConstraintPointOnLine(*p, *l, tag);
                break;
            }
            case ConstraintKind::EqualLength: {
                auto* l1 = gcsLine(c.a);
                auto* l2 = gcsLine(c.b);
                if (l1 != nullptr && l2 != nullptr) sys.addConstraintEqualLength(*l1, *l2, tag);
                break;
            }
            case ConstraintKind::LockX:
                if (auto* p = gcsPoint(c.a, c.aPoint)) {
                    sys.addConstraintCoordinateX(*p, value(c.value), tag);
                }
                break;
            case ConstraintKind::LockY:
                if (auto* p = gcsPoint(c.a, c.aPoint)) {
                    sys.addConstraintCoordinateY(*p, value(c.value), tag);
                }
                break;
        }
    }

    sys.declareUnknowns(unknowns);
    sys.initSolution();

    // diagnose() BEFORE solve(). It is what fills the conflict and redundancy report and computes
    // the DOF count; solve() alone leaves them stale, so reporting without it means telling the user
    // a contradictory sketch is fine. See modules/planegcs/VENDORED.md.
    sys.diagnose();
    report.dofs = sys.dofsNumber();

    GCS::VEC_I conflicting;
    GCS::VEC_I redundant;
    sys.getConflicting(conflicting);
    sys.getRedundant(redundant);
    // Tags back to constraint indices, undoing the 1-based offset above.
    for (const int tag : conflicting) {
        if (tag > 0) report.conflicting.push_back(static_cast<std::size_t>(tag - 1));
    }
    for (const int tag : redundant) {
        if (tag > 0) report.redundant.push_back(static_cast<std::size_t>(tag - 1));
    }

    const int status = sys.solve();
    report.solved = status == GCS::Success || status == GCS::Converged;
    if (report.solved) {
        sys.applySolution();
        // Copy the solved parameters back. The solver wrote through the pointers we handed it, so
        // the GCS objects hold the answer and our Geometry has to be refreshed from them.
        for (std::size_t i = 0; i < geometry_.size(); ++i) {
            Geometry& g = geometry_[i];
            const Mapping& m = mapping[i];
            switch (m.kind) {
                case GeoKind::Point:
                    g.p[0] = *points[m.index].x;
                    g.p[1] = *points[m.index].y;
                    break;
                case GeoKind::Line:
                    g.p[0] = *lines[m.index].p1.x;
                    g.p[1] = *lines[m.index].p1.y;
                    g.p[2] = *lines[m.index].p2.x;
                    g.p[3] = *lines[m.index].p2.y;
                    break;
                case GeoKind::Circle:
                    g.p[0] = *circles[m.index].center.x;
                    g.p[1] = *circles[m.index].center.y;
                    g.p[2] = *circles[m.index].rad;
                    break;
                case GeoKind::Arc: {
                    g.p[0] = *arcs[m.index].center.x;
                    g.p[1] = *arcs[m.index].center.y;
                    g.p[2] = *arcs[m.index].rad;
                    // Angles derived from the solved ENDPOINTS rather than read from the angle
                    // parameters. Both are correct today: the angles are declared as unknowns above
                    // and addConstraintArcRules keeps them consistent with the endpoints, so
                    // planegcs updates them itself. This is the more robust of the two -- the
                    // endpoints are what other constraints actually act on, so deriving from them
                    // cannot drift if a solve leaves a small residual in the angle. Kept for that
                    // reason, not because the parameters were stale; see the arc test in
                    // m5_sketch.rs, which passes either way.
                    const double dx1 = *arcs[m.index].start.x - g.p[0];
                    const double dy1 = *arcs[m.index].start.y - g.p[1];
                    const double dx2 = *arcs[m.index].end.x - g.p[0];
                    const double dy2 = *arcs[m.index].end.y - g.p[1];
                    g.p[3] = std::atan2(dy1, dx1);
                    g.p[4] = std::atan2(dy2, dx2);
                    break;
                }
            }
        }
    }

    // A solve that produced a non-finite coordinate did NOT succeed.
    //
    // planegcs reported solved=1 over geometry containing NaN, because a NaN propagates through
    // its residuals without ever comparing greater than the convergence tolerance — every
    // comparison with NaN is false, so the loop reads "converged". The NaN itself arrives either
    // from input we accepted (a line typed with a bad coordinate) or from a diverging solve.
    //
    // The geometry is REVERTED rather than left as it is. Leaving NaN in the sketch poisons every
    // later solve, the DXF export, the serialised document and any extrude built on the profile —
    // and each of those would then fail somewhere far from the cause. A sketch that refuses an
    // edit and stays as it was is one the user can carry on working in.
    //
    // Found by the geometry torture suite; see tests-rs/cad-bench/tests/torture.rs.
    {
        bool finite = true;
        for (const Geometry& g : geometry_) {
            for (const double v : g.p) {
                if (!std::isfinite(v)) { finite = false; break; }
            }
            if (!finite) break;
        }
        if (!finite) {
            geometry_ = before;
            report.solved = false;
            report.dofs = 0;
            report.message =
                "The sketch could not be solved: the solution contained a value that is not a "
                "number. The sketch is unchanged.";
            return report;
        }
    }

    if (!report.conflicting.empty()) {
        report.message = "The sketch is over-constrained: "
                         + std::to_string(report.conflicting.size())
                         + " constraints cannot all hold.";
    } else if (!report.solved) {
        report.message = "The sketch could not be solved.";
    } else if (report.dofs > 0) {
        report.message = std::to_string(report.dofs) + " degrees of freedom remaining.";
    } else {
        report.message = "Fully constrained.";
    }
    return report;
}

// ── geometry out ────────────────────────────────────────────────────────────────────────

std::array<double, 3> Sketch::to3d(double u, double v) const {
    switch (plane_) {
        case Plane::XY: return {u, v, 0.0};
        case Plane::XZ: return {u, 0.0, v};
        case Plane::YZ: return {0.0, u, v};
    }
    return {u, v, 0.0};
}

kernel::Result<kernel::Shape> Sketch::toWire() const {
    // guard() wraps whatever the lambda returns, so a lambda returning Result<Shape> gives back a
    // Result<Result<Shape>>. Flattened here rather than by having the lambda return a bare Shape,
    // because the failure cases below are ours (open profile, disconnected curves) and deserve
    // their own messages -- not an OCCT exception translated into a generic kernel error.
    auto guarded = kernel::guard("build the sketch profile",
                                 [&]() -> kernel::Result<kernel::Shape> {
        const auto point = [&](double u, double v) {
            const auto p = to3d(u, v);
            return gp_Pnt(p[0], p[1], p[2]);
        };
        // Plane normal, for circles and arcs: the axis they are swept about.
        const gp_Dir normal = plane_ == Plane::XY ? gp_Dir(0, 0, 1)
                              : plane_ == Plane::XZ ? gp_Dir(0, -1, 0)
                                                    : gp_Dir(1, 0, 0);

        BRepBuilderAPI_MakeWire wire;
        int used = 0;
        for (const Geometry& g : geometry_) {
            if (g.construction || g.kind == GeoKind::Point) continue;
            switch (g.kind) {
                case GeoKind::Line: {
                    const gp_Pnt a = point(g.p[0], g.p[1]);
                    const gp_Pnt b = point(g.p[2], g.p[3]);
                    if (a.Distance(b) < 1e-9) continue;   // degenerate; contributes no edge
                    wire.Add(BRepBuilderAPI_MakeEdge(a, b).Edge());
                    ++used;
                    break;
                }
                case GeoKind::Circle: {
                    const auto c = to3d(g.p[0], g.p[1]);
                    const gp_Circ circle(gp_Ax2(gp_Pnt(c[0], c[1], c[2]), normal), g.p[2]);
                    wire.Add(BRepBuilderAPI_MakeEdge(circle).Edge());
                    ++used;
                    break;
                }
                case GeoKind::Arc: {
                    const gp_Pnt start = point(g.p[0] + g.p[2] * std::cos(g.p[3]),
                                               g.p[1] + g.p[2] * std::sin(g.p[3]));
                    const gp_Pnt end = point(g.p[0] + g.p[2] * std::cos(g.p[4]),
                                             g.p[1] + g.p[2] * std::sin(g.p[4]));
                    const double mid = (g.p[3] + g.p[4]) * 0.5;
                    const gp_Pnt through = point(g.p[0] + g.p[2] * std::cos(mid),
                                                 g.p[1] + g.p[2] * std::sin(mid));
                    // Through three points rather than centre-and-angles: it gets the direction
                    // right without a separate reversed/normal case, and the midpoint is exactly
                    // the information that disambiguates a major from a minor arc.
                    GC_MakeArcOfCircle arc(start, through, end);
                    if (!arc.IsDone()) continue;
                    wire.Add(BRepBuilderAPI_MakeEdge(arc.Value()).Edge());
                    ++used;
                    break;
                }
                case GeoKind::Point: break;
            }
        }

        if (used == 0) {
            return Error{ErrorCode::InvalidInput,
                         "This sketch has no profile geometry.",
                         "only points or construction geometry"};
        }
        if (!wire.IsDone()) {
            // MakeWire fails when edges do not connect. That is the common real mistake — a corner
            // left unjoined — so name it rather than reporting a generic build failure.
            return Error{ErrorCode::InvalidResult,
                         "The sketch profile is not connected. Its curves must meet end to end.",
                         "BRepBuilderAPI_MakeWire::IsDone() == false"};
        }
        TopoDS_Wire result = wire.Wire();
        if (!result.Closed()) {
            return Error{ErrorCode::InvalidInput,
                         "The sketch profile is open. A closed profile is needed to build a solid."};
        }
        return kernel::wrap(result);
    });
    if (!guarded) return guarded.error();
    return guarded.value();
}

kernel::Result<kernel::Shape> Sketch::toFace() const {
    auto wire = toWire();
    if (!wire) return wire.error();
    auto guarded = kernel::guard("build the sketch face",
                                 [&]() -> kernel::Result<kernel::Shape> {
        BRepBuilderAPI_MakeFace face(TopoDS::Wire(kernel::occt(wire.value())), /*OnlyPlane*/ true);
        if (!face.IsDone()) {
            return Error{ErrorCode::InvalidResult,
                         "The sketch profile does not bound a flat area."};
        }
        return kernel::wrap(face.Face());
    });
    if (!guarded) return guarded.error();
    return guarded.value();
}

// ── persistence ─────────────────────────────────────────────────────────────────────────

std::string Sketch::serialize() const {
    // Text, one record per line, version first. Readable in the sqlite3 CLI and diffable in a
    // review, both of which a binary blob costs us for no gain at this size.
    std::ostringstream out;
    out.precision(17);   // exact round-trip for IEEE-754 doubles
    out << "sketch 1\n";
    out << "plane " << toString(plane_) << '\n';
    for (std::size_t i = 0; i < geometry_.size(); ++i) {
        const Geometry& g = geometry_[i];
        out << "g " << ids_[i] << ' ' << toString(g.kind) << ' ' << (g.construction ? 1 : 0);
        for (std::size_t k = 0; k < paramCount(g.kind); ++k) out << ' ' << g.p[k];
        out << '\n';
    }
    for (const Constraint& c : constraints_) {
        out << "c " << toString(c.kind) << ' ' << c.a << ' ' << static_cast<int>(c.aPoint) << ' '
            << c.b << ' ' << static_cast<int>(c.bPoint) << ' ' << c.value << '\n';
    }
    return out.str();
}

kernel::Result<Sketch> Sketch::deserialize(std::string_view text) {
    Sketch sketch;
    // BRACES, not parentheses. `std::istringstream in(std::string(text));` is the most vexing
    // parse: it declares a FUNCTION named `in` taking a std::string and returning a stream, and
    // the only symptom is getline failing to match anything.
    std::istringstream in{std::string(text)};
    std::string line;

    if (!std::getline(in, line)) {
        return Error{ErrorCode::InvalidInput, "That sketch is empty."};
    }
    {
        std::istringstream head(line);
        std::string tag;
        int version = 0;
        head >> tag >> version;
        if (tag != "sketch") {
            return Error{ErrorCode::InvalidInput, "That is not a sketch."};
        }
        if (version > 1) {
            return Error{ErrorCode::Unsupported,
                         "This sketch was written by a newer version of vCAD.",
                         "sketch format version " + std::to_string(version)};
        }
    }

    GeoId highest = 0;
    bool any = false;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        std::istringstream row(line);
        std::string tag;
        row >> tag;

        if (tag == "plane") {
            std::string name;
            row >> name;
            sketch.plane_ = name == "XZ" ? Plane::XZ : name == "YZ" ? Plane::YZ : Plane::XY;
        } else if (tag == "g") {
            GeoId id = 0;
            std::string kindName;
            int construction = 0;
            row >> id >> kindName >> construction;

            Geometry g;
            if (kindName == "line") g.kind = GeoKind::Line;
            else if (kindName == "circle") g.kind = GeoKind::Circle;
            else if (kindName == "arc") g.kind = GeoKind::Arc;
            else if (kindName == "point") g.kind = GeoKind::Point;
            else {
                return Error{ErrorCode::Unsupported,
                             "This sketch contains geometry we do not understand.", kindName};
            }
            g.construction = construction != 0;
            for (std::size_t k = 0; k < paramCount(g.kind); ++k) row >> g.p[k];

            // Ids are preserved rather than reassigned, for the same reason document object ids
            // are: constraints refer to them, and renumbering silently re-points every constraint.
            sketch.geometry_.push_back(g);
            sketch.ids_.push_back(id);
            highest = std::max(highest, id);
            any = true;
        } else if (tag == "c") {
            std::string kindName;
            Constraint c;
            int ap = 0;
            int bp = 0;
            row >> kindName >> c.a >> ap >> c.b >> bp >> c.value;
            c.aPoint = static_cast<PointRef>(ap);
            c.bPoint = static_cast<PointRef>(bp);

            bool known = true;
            if (kindName == "coincident") c.kind = ConstraintKind::Coincident;
            else if (kindName == "horizontal") c.kind = ConstraintKind::Horizontal;
            else if (kindName == "vertical") c.kind = ConstraintKind::Vertical;
            else if (kindName == "parallel") c.kind = ConstraintKind::Parallel;
            else if (kindName == "perpendicular") c.kind = ConstraintKind::Perpendicular;
            else if (kindName == "distance") c.kind = ConstraintKind::Distance;
            else if (kindName == "radius") c.kind = ConstraintKind::Radius;
            else if (kindName == "pointonline") c.kind = ConstraintKind::PointOnLine;
            else if (kindName == "equallength") c.kind = ConstraintKind::EqualLength;
            else if (kindName == "lockx") c.kind = ConstraintKind::LockX;
            else if (kindName == "locky") c.kind = ConstraintKind::LockY;
            else known = false;

            if (!known) {
                // Refusing beats dropping. A silently discarded constraint leaves a sketch that
                // solves to different geometry than the one that was saved.
                return Error{ErrorCode::Unsupported,
                             "This sketch contains a constraint we do not understand.", kindName};
            }
            sketch.constraints_.push_back(c);
        }
    }

    sketch.nextId_ = any ? highest + 1 : 0;
    return sketch;
}

}  // namespace cad::sketch
