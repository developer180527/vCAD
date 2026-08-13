#include "cad/sketch/Infer.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

namespace cad::sketch {
namespace {

/// One endpoint of one geometry — the unit coincidence works on.
struct Endpoint {
    GeoId geo = 0;
    PointRef ref = PointRef::Start;
    double x = 0.0;
    double y = 0.0;
};

/// Union-find over endpoints.
///
/// Needed because coincidence is transitive and overlapping: if A touches B and B touches C, all
/// three are one point even when A and C are further apart than the tolerance. Handling that with
/// pairwise comparisons alone produces an inconsistent set of constraints — and a solver fed
/// inconsistent coincidences reports a conflict on a sketch that is perfectly reasonable.
class Groups {
public:
    explicit Groups(std::size_t n) : parent_(n) {
        for (std::size_t i = 0; i < n; ++i) parent_[i] = i;
    }
    std::size_t find(std::size_t i) {
        while (parent_[i] != i) {
            parent_[i] = parent_[parent_[i]];   // path halving
            i = parent_[i];
        }
        return i;
    }
    bool join(std::size_t a, std::size_t b) {
        const std::size_t ra = find(a);
        const std::size_t rb = find(b);
        if (ra == rb) return false;
        parent_[rb] = ra;
        return true;
    }

private:
    std::vector<std::size_t> parent_;
};

/// Line direction as an angle in [0, 180). Modulo 180 because a line has no direction for these
/// purposes: a line at 181 degrees is the same line as one at 1 degree, and treating them as
/// different would miss half of all horizontal lines.
double lineAngleDeg(const Geometry& g) {
    double deg = std::atan2(g.p[3] - g.p[1], g.p[2] - g.p[0]) * 180.0 / std::numbers::pi;
    deg = std::fmod(deg, 180.0);
    if (deg < 0.0) deg += 180.0;
    return deg;
}

bool isDegenerateLine(const Geometry& g) {
    const double dx = g.p[2] - g.p[0];
    const double dy = g.p[3] - g.p[1];
    return dx * dx + dy * dy < 1e-18;
}

}  // namespace

std::string InferenceReport::summary() const {
    std::string text = std::to_string(added()) + " constraints inferred";
    if (coincident > 0) text += ", " + std::to_string(coincident) + " coincident";
    if (horizontal > 0) text += ", " + std::to_string(horizontal) + " horizontal";
    if (vertical > 0) text += ", " + std::to_string(vertical) + " vertical";
    if (parallel > 0) text += ", " + std::to_string(parallel) + " parallel";
    if (perpendicular > 0) text += ", " + std::to_string(perpendicular) + " perpendicular";
    text += ". Degrees of freedom " + std::to_string(dofsBefore) + " -> "
            + std::to_string(dofsAfter);
    if (conflicting > 0) {
        text += ". WARNING: " + std::to_string(conflicting)
                + " constraints conflict — the tolerance may be too loose.";
    }
    return text;
}

InferenceReport infer(Sketch& sketch, const InferenceOptions& options) {
    InferenceReport report;

    // Baseline DOF, measured before adding anything, so the report can show what inference bought.
    // A copy, because solve() moves geometry to satisfy whatever constraints exist and we do not
    // want a measurement to change the sketch.
    {
        Sketch before = sketch;
        report.dofsBefore = before.solve().dofs;
    }

    const auto& geometry = sketch.geometry();

    const auto& ids = sketch.ids();


    // ── coincidence ─────────────────────────────────────────────────────────────────────
    if (options.coincident) {
        std::vector<Endpoint> endpoints;
        endpoints.reserve(geometry.size() * 2);
        for (std::size_t i = 0; i < geometry.size(); ++i) {
            const Geometry& g = geometry[i];
            // Circles have no endpoints, and construction geometry participates: a centreline whose
            // end meets a profile corner is exactly the sort of relationship worth keeping.
            if (g.kind == GeoKind::Circle || g.kind == GeoKind::Point) continue;
            for (const PointRef ref : {PointRef::Start, PointRef::End}) {
                if (auto p = sketch.pointAt(ids[i], ref)) {
                    endpoints.push_back({ids[i], ref, p.value()[0], p.value()[1]});
                }
            }
        }

        Groups groups(endpoints.size());
        const double tol2 = options.pointTolerance * options.pointTolerance;

        // Sorted by x so the inner loop can stop early. O(n^2) worst case but O(n log n) in
        // practice on real geometry, and a full spatial index is not worth its complexity until a
        // sketch has thousands of endpoints — which is a different problem than this.
        std::vector<std::size_t> order(endpoints.size());
        for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
            return endpoints[a].x < endpoints[b].x;
        });

        for (std::size_t oa = 0; oa < order.size(); ++oa) {
            const Endpoint& a = endpoints[order[oa]];
            for (std::size_t ob = oa + 1; ob < order.size(); ++ob) {
                const Endpoint& b = endpoints[order[ob]];
                const double dx = b.x - a.x;
                if (dx > options.pointTolerance) break;   // sorted: nothing further can match
                const double dy = b.y - a.y;
                if (dx * dx + dy * dy > tol2) continue;
                // Never constrain an endpoint to the other end of the SAME line: that would demand
                // a zero-length line, which is unsatisfiable.
                if (a.geo == b.geo) continue;
                // join() returns false when they are already in one group, and skipping those is
                // precisely what keeps a k-point cluster at k-1 constraints instead of k(k-1)/2.
                if (groups.join(order[oa], order[ob])) {
                    sketch.coincident(a.geo, a.ref, b.geo, b.ref);
                    ++report.coincident;
                }
            }
        }
    }

    // ── horizontal / vertical ───────────────────────────────────────────────────────────
    //
    // Recorded per line, because parallel/perpendicular below must not re-state what these already
    // imply.
    std::vector<bool> axisAligned(geometry.size(), false);
    if (options.horizontalVertical) {
        for (std::size_t i = 0; i < geometry.size(); ++i) {
            const Geometry& g = geometry[i];
            if (g.kind != GeoKind::Line || isDegenerateLine(g)) continue;
            const double angle = lineAngleDeg(g);
            if (angle <= options.angleToleranceDeg
                || angle >= 180.0 - options.angleToleranceDeg) {
                sketch.horizontal(ids[i]);
                ++report.horizontal;
                axisAligned[i] = true;
            } else if (std::abs(angle - 90.0) <= options.angleToleranceDeg) {
                sketch.vertical(ids[i]);
                ++report.vertical;
                axisAligned[i] = true;
            }
        }
    }

    // ── parallel / perpendicular ────────────────────────────────────────────────────────
    if (options.parallelPerpendicular) {
        for (std::size_t i = 0; i < geometry.size(); ++i) {
            if (geometry[i].kind != GeoKind::Line || isDegenerateLine(geometry[i])) continue;
            if (axisAligned[i]) continue;   // already fixed against the axes; see the header
            for (std::size_t j = i + 1; j < geometry.size(); ++j) {
                if (geometry[j].kind != GeoKind::Line || isDegenerateLine(geometry[j])) continue;
                if (axisAligned[j]) continue;
                const double delta = std::abs(lineAngleDeg(geometry[i]) - lineAngleDeg(geometry[j]));
                if (delta <= options.angleToleranceDeg
                    || delta >= 180.0 - options.angleToleranceDeg) {
                    sketch.parallel(ids[i], ids[j]);
                    ++report.parallel;
                } else if (std::abs(delta - 90.0) <= options.angleToleranceDeg) {
                    sketch.perpendicular(ids[i], ids[j]);
                    ++report.perpendicular;
                }
            }
        }
    }

    // Solve for real: inference is only worth anything if the result still solves, and the DOF and
    // conflict figures have to come from an actual solve rather than from counting what we added.
    const SolveReport solved = sketch.solve();
    report.dofsAfter = solved.dofs;
    report.conflicting = solved.conflicting.size();
    report.redundant = solved.redundant.size();
    return report;
}

}  // namespace cad::sketch
