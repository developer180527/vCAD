// Spike: does the vendored solver actually solve?
//
// A library that compiles and links proves nothing about a constraint solver. This drives the real
// thing: two points, a line between them, and constraints that force a known answer. If the numbers
// below come out right, planegcs is usable and the sketcher can be built on it.
//
// Deliberately NOT a unit test yet. Tests go through the C ABI in Rust (ADR 0006), and there is no
// sketch API to expose. This is the de-risking step that docs/STATUS.md recorded as never done.

#include "GCS.h"

#include <cmath>
#include <cstdio>
#include <vector>

int main() {
    // Parameters ARE the sketch's degrees of freedom: the solver mutates these doubles in place.
    // Held in a deque-like stable container because GCS stores POINTERS to them — a vector that
    // reallocates would leave the solver writing into freed memory.
    std::vector<double*> params;
    auto param = [&params](double initial) {
        auto* p = new double(initial);
        params.push_back(p);
        return p;
    };

    GCS::Point a;
    a.x = param(0.0);
    a.y = param(0.0);
    GCS::Point b;
    b.x = param(30.0);   // deliberately wrong: the solver must move it to satisfy the length
    b.y = param(40.0);

    GCS::Line line;
    line.p1 = a;
    line.p2 = b;

    GCS::System sys;
    // Pin A at the origin, force the line horizontal, and demand it be exactly 100 long.
    // Answer: B = (100, 0). Nothing here tells the solver that directly.
    sys.addConstraintCoordinateX(a, param(0.0), 1);
    sys.addConstraintCoordinateY(a, param(0.0), 2);
    sys.addConstraintHorizontal(line, 3);
    sys.addConstraintP2PDistance(a, b, param(100.0), 4);

    std::vector<double*> unknowns{a.x, a.y, b.x, b.y};
    sys.declareUnknowns(unknowns);
    sys.initSolution();

    const int status = sys.solve();
    const char* name = status == GCS::Success   ? "Success"
                       : status == GCS::Converged ? "Converged"
                       : status == GCS::Failed    ? "Failed"
                                                  : "SuccessfulSolutionInvalid";
    std::printf("solve: %s (%d)   dofs: %d\n", name, status, sys.dofsNumber());
    if (status == GCS::Success || status == GCS::Converged) sys.applySolution();

    std::printf("A = (%.6f, %.6f)\nB = (%.6f, %.6f)\n", *a.x, *a.y, *b.x, *b.y);

    const double length = std::hypot(*b.x - *a.x, *b.y - *a.y);
    std::printf("length = %.6f (want 100)\n", length);

    bool ok = status == GCS::Success || status == GCS::Converged;
    ok = ok && std::abs(*a.x) < 1e-9 && std::abs(*a.y) < 1e-9;
    ok = ok && std::abs(*b.y - *a.y) < 1e-9;          // horizontal
    ok = ok && std::abs(length - 100.0) < 1e-6;       // and the right length

    // The other half of a usable solver: it must TELL you when a sketch is over-constrained rather
    // than silently picking one interpretation. Add a second, contradictory length.
    GCS::System over;
    over.addConstraintCoordinateX(a, param(0.0), 1);
    over.addConstraintCoordinateY(a, param(0.0), 2);
    over.addConstraintHorizontal(line, 3);
    over.addConstraintP2PDistance(a, b, param(100.0), 4);
    over.addConstraintP2PDistance(a, b, param(80.0), 5);   // cannot also be 80
    over.declareUnknowns(unknowns);
    over.initSolution();
    // diagnose() is what populates the conflict/redundancy report; solve() alone leaves it stale.
    // That is the API detail a sketcher has to get right, or the UI shows "solved" for a sketch the
    // solver knows is contradictory.
    over.diagnose();
    const int overStatus = over.solve();
    GCS::VEC_I conflicting;
    GCS::VEC_I redundant;
    over.getConflicting(conflicting);
    over.getRedundant(redundant);
    std::printf("over-constrained: status %d, dofs %d, conflicting %zu, redundant %zu\n",
                overStatus, over.dofsNumber(), conflicting.size(), redundant.size());
    const bool detected =
        overStatus == GCS::Failed || !conflicting.empty() || !redundant.empty();
    if (!detected) std::printf("WARN: a contradictory sketch was not reported as such\n");

    for (double* p : params) delete p;
    std::printf(ok && detected ? "OK\n" : "FAIL\n");
    return ok && detected ? 0 : 1;
}
