// Spike: core/sketch, end to end.
//
// A rectangle drawn badly, then constrained into shape, then turned into an OCCT face. That is the
// whole path a sketch-based feature depends on: our API -> planegcs -> our geometry -> OCCT.
//
// The corners start at deliberately wrong coordinates. Nothing tells the solver where they belong;
// it derives that from "these corners meet", "these edges are horizontal/vertical", and two
// dimensions. If the area comes out at exactly 80 x 50, the sketcher's foundation works.

#include "cad/sketch/Sketch.h"
#include "cad/kernel/Shape.h"

#include <cstdio>
#include <cmath>

using namespace cad;
using cad::sketch::PointRef;

int main() {
    sketch::Sketch s(sketch::Plane::XY);

    // Four lines, corners nowhere near each other.
    const auto bottom = s.addLine(0, 0, 63, 7);
    const auto right = s.addLine(63, 7, 71, 44);
    const auto top = s.addLine(71, 44, -5, 39);
    const auto left = s.addLine(-5, 39, 0, 0);

    // Close the loop.
    s.coincident(bottom, PointRef::End, right, PointRef::Start);
    s.coincident(right, PointRef::End, top, PointRef::Start);
    s.coincident(top, PointRef::End, left, PointRef::Start);
    s.coincident(left, PointRef::End, bottom, PointRef::Start);

    // Make it a rectangle.
    s.horizontal(bottom);
    s.horizontal(top);
    s.vertical(left);
    s.vertical(right);

    // Pin it to the origin, or the whole sketch floats: a rectangle constrained only in shape has
    // three degrees of freedom left (x, y, and nothing else here — rotation is fixed by horizontal).
    s.lockX(bottom, PointRef::Start, 0.0);
    s.lockY(bottom, PointRef::Start, 0.0);

    // Two dimensions.
    s.distance(bottom, PointRef::Start, bottom, PointRef::End, 80.0);
    s.distance(left, PointRef::End, top, PointRef::End, 50.0);

    const auto report = s.solve();
    std::printf("solve: %s   dofs: %d   %s\n", report.solved ? "ok" : "FAILED", report.dofs,
                report.message.c_str());
    for (const auto& g : s.geometry()) {
        std::printf("  line (%7.3f, %7.3f) -> (%7.3f, %7.3f)\n", g.p[0], g.p[1], g.p[2], g.p[3]);
    }

    bool ok = report.solved && report.dofs == 0 && report.conflicting.empty();

    // Width and height, read back from the solved geometry rather than from what we asked for.
    const auto* b = s.find(bottom);
    const auto* l = s.find(left);
    const double width = std::hypot(b->p[2] - b->p[0], b->p[3] - b->p[1]);
    const double height = std::hypot(l->p[2] - l->p[0], l->p[3] - l->p[1]);
    std::printf("width  = %.6f (want 80)\nheight = %.6f (want 50)\n", width, height);
    ok = ok && std::abs(width - 80.0) < 1e-6 && std::abs(height - 50.0) < 1e-6;

    // The OCCT bridge: a closed wire, then a planar face. This is what Extrude will consume.
    auto face = s.toFace();
    if (!face) {
        std::printf("toFace FAILED: %s (%s)\n", face.error().message.c_str(),
                    face.error().detail.c_str());
        ok = false;
    } else {
        const auto valid = face.value().validate();
        std::printf("face: type %s, valid %s\n", kernel::toString(face.value().type()),
                    valid ? "yes" : valid.error().message.c_str());
        ok = ok && static_cast<bool>(valid);
    }

    // An open profile must be refused rather than silently producing a broken solid.
    {
        sketch::Sketch open(sketch::Plane::XY);
        open.addLine(0, 0, 10, 0);
        open.addLine(10, 0, 10, 10);
        const auto refused = open.toFace();
        std::printf("open profile refused: %s\n", refused ? "NO — accepted!" : "yes");
        if (refused) ok = false;
    }

    // Round-trip through the text form the document stores.
    {
        const std::string text = s.serialize();
        auto back = sketch::Sketch::deserialize(text);
        if (!back) {
            std::printf("deserialize FAILED: %s\n", back.error().message.c_str());
            ok = false;
        } else {
            const bool same = back.value().serialize() == text
                              && back.value().geometry().size() == s.geometry().size()
                              && back.value().constraints().size() == s.constraints().size();
            std::printf("round-trip: %s (%zu bytes, %zu geo, %zu constraints)\n",
                        same ? "identical" : "DIFFERS", text.size(),
                        back.value().geometry().size(), back.value().constraints().size());
            ok = ok && same;
            // And it must still solve after a round trip — ids and constraint targets survived.
            auto again = back.value();
            const auto r2 = again.solve();
            std::printf("re-solve after round-trip: %s dofs %d\n", r2.solved ? "ok" : "FAILED",
                        r2.dofs);
            ok = ok && r2.solved && r2.dofs == 0;
        }
    }

    std::printf(ok ? "OK\n" : "FAIL\n");
    return ok ? 0 : 1;
}
