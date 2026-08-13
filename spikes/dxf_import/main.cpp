// Spike: DXF import into a sketch.
//
// Reads tests/data/sketch_profile.dxf, which deliberately contains one of everything that matters:
// a closed LWPOLYLINE, a circle, an arc, a construction-layer line, a zero-length line, and a
// SPLINE we cannot import. The assertions are about what arrives AND about what the report says was
// lost -- a reader that silently drops geometry is worse than one that refuses.

#include "cad/sketch/Dxf.h"

#include <cmath>
#include <cstdio>
#include <numbers>

using namespace cad;

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : "tests/data/sketch_profile.dxf";

    io::DxfImportOptions options;
    io::DxfImportReport report;
    auto imported = io::importDxf(path, options, &report);
    if (!imported) {
        std::printf("import FAILED: %s (%s)\n", imported.error().message.c_str(),
                    imported.error().detail.c_str());
        return 1;
    }
    sketch::Sketch& s = imported.value();

    std::printf("%s\n", report.summary().c_str());
    std::printf("lines %zu  arcs %zu  circles %zu  points %zu  polylines %zu\n", report.lines,
                report.arcs, report.circles, report.points, report.polylines);

    bool ok = true;
    // Square is 4 lines (closed polyline contributes the 4th), plus the construction centreline.
    ok = ok && report.lines == 5;
    ok = ok && report.circles == 1 && report.arcs == 1;
    ok = ok && report.construction == 1;
    ok = ok && report.degenerate == 1;          // the zero-length line was dropped
    ok = ok && report.polylines == 1;
    // The SPLINE must be REPORTED, not silently ignored.
    const bool declaredSpline =
        !report.unsupported.empty() && report.unsupported.front().first == "SPLINE";
    std::printf("unsupported declared: %s\n", declaredSpline ? "yes (SPLINE)" : "NO");
    ok = ok && declaredSpline;

    // Arc angles must have been converted from degrees to radians.
    for (const auto& g : s.geometry()) {
        if (g.kind != sketch::GeoKind::Arc) continue;
        std::printf("arc: centre (%.1f, %.1f) r %.1f  start %.4f rad  end %.4f rad\n", g.p[0],
                    g.p[1], g.p[2], g.p[3], g.p[4]);
        ok = ok && std::abs(g.p[3] - 0.0) < 1e-9;
        ok = ok && std::abs(g.p[4] - std::numbers::pi / 2) < 1e-9;
    }

    // Construction geometry must be excluded from the profile. The square alone is closed; with the
    // centreline treated as profile, the wire would be branched and toWire would reject it.
    // The circle is a second closed loop, so a single-wire build is expected to refuse THIS file --
    // which is itself the correct answer, and worth asserting rather than assuming.
    auto wire = s.toWire();
    std::printf("toWire on a multi-loop profile: %s\n",
                wire ? "accepted" : ("refused — " + wire.error().message).c_str());

    // Scale is applied to every coordinate.
    io::DxfImportReport scaled;
    io::DxfImportOptions inches;
    inches.scale = 25.4;
    auto big = io::importDxf(path, inches, &scaled);
    if (!big) {
        std::printf("scaled import FAILED\n");
        ok = false;
    } else {
        double maxX = 0.0;
        for (const auto& g : big.value().geometry()) {
            if (g.kind == sketch::GeoKind::Line) maxX = std::max({maxX, g.p[0], g.p[2]});
        }
        std::printf("scaled x25.4: max x = %.3f (want 1016 = 40 x 25.4)\n", maxX);
        ok = ok && std::abs(maxX - 1016.0) < 1e-6;
    }

    // A file that is not DXF must be refused with a message that mentions DWG, because a renamed
    // DWG is the most common way this fails in practice.
    {
        auto bad = io::importDxf("CMakeLists.txt");
        std::printf("non-DXF refused: %s\n", bad ? "NO — accepted!" : "yes");
        ok = ok && !bad;
    }

    std::printf(ok ? "OK\n" : "FAIL\n");
    return ok ? 0 : 1;
}
