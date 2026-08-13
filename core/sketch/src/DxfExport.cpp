#include "cad/sketch/Dxf.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <numbers>

namespace cad::io {
namespace {

using kernel::Error;
using kernel::ErrorCode;

constexpr double kRadToDeg = 180.0 / std::numbers::pi;

/// A DXF file is a stream of (group code, value) PAIRS, one per line. Everything below is that:
/// `pair(0, "LINE")` starts an entity, codes 10/20/30 are the first point, 11/21/31 the second.
void pair(std::ostream& out, int code, const std::string& value) {
    out << code << '\n' << value << '\n';
}

/// Coordinates are written with 17 significant digits.
///
/// Not cosmetic: an exported sketch is often re-imported, and %.6f silently rounds a solved
/// position. A sketch that solved to a corner exactly on the origin should come back on the origin,
/// not at 1e-7.
void pair(std::ostream& out, int code, double value) {
    char buffer[40];
    std::snprintf(buffer, sizeof buffer, "%.17g", value);
    out << code << '\n' << buffer << '\n';
}

void pair(std::ostream& out, int code, int value) {
    out << code << '\n' << value << '\n';
}

}  // namespace

kernel::Result<void> exportDxf(const sketch::Sketch& sketch, const std::filesystem::path& path,
                              const DxfExportOptions& options) {
    if (!(options.scale > 0.0) || !std::isfinite(options.scale)) {
        return Error{ErrorCode::InvalidInput, "The export scale must be a positive number."};
    }

    // Write to a temporary and rename, as saveDocument does: a crash mid-write must not leave a
    // truncated file where an existing export used to be.
    std::filesystem::path temp = path;
    temp += ".writing";
    std::error_code ignored;
    std::filesystem::remove(temp, ignored);

    {
        std::ofstream out(temp, std::ios::binary);
        if (!out) {
            return Error{ErrorCode::InvalidInput, "That file could not be written.",
                         path.string()};
        }

        // ── HEADER ──────────────────────────────────────────────────────────────────────
        pair(out, 0, std::string("SECTION"));
        pair(out, 2, std::string("HEADER"));
        pair(out, 9, std::string("$ACADVER"));
        pair(out, 1, std::string("AC1009"));   // R12
        // $INSUNITS = 4 (millimetres). We ignore this on IMPORT because the wild is full of files
        // that lie about it — but writing it correctly costs nothing and helps everyone else.
        pair(out, 9, std::string("$INSUNITS"));
        pair(out, 70, 4);
        pair(out, 0, std::string("ENDSEC"));

        // ── TABLES: layers ─────────────────────────────────────────────────────────────
        // R12 readers are entitled to reject an entity on an undeclared layer, so the construction
        // layer has to exist here even though the entities below name it.
        pair(out, 0, std::string("SECTION"));
        pair(out, 2, std::string("TABLES"));
        pair(out, 0, std::string("TABLE"));
        pair(out, 2, std::string("LAYER"));
        pair(out, 70, 2);
        for (const auto& [name, colour] : {std::pair{options.profileLayer, 7},
                                           std::pair{options.constructionLayer, 8}}) {
            pair(out, 0, std::string("LAYER"));
            pair(out, 2, name);
            pair(out, 70, 0);
            pair(out, 62, colour);   // 7 = white/black, 8 = dark grey, the usual construction colour
            pair(out, 6, std::string("CONTINUOUS"));
        }
        pair(out, 0, std::string("ENDTAB"));
        pair(out, 0, std::string("ENDSEC"));

        // ── ENTITIES ───────────────────────────────────────────────────────────────────
        pair(out, 0, std::string("SECTION"));
        pair(out, 2, std::string("ENTITIES"));

        const double k = 1.0 / options.scale;
        std::size_t written = 0;
        for (const sketch::Geometry& g : sketch.geometry()) {
            if (g.construction && !options.includeConstruction) continue;
            const std::string layer =
                g.construction ? options.constructionLayer : options.profileLayer;

            switch (g.kind) {
                case sketch::GeoKind::Line:
                    pair(out, 0, std::string("LINE"));
                    pair(out, 8, layer);
                    pair(out, 10, g.p[0] * k);
                    pair(out, 20, g.p[1] * k);
                    pair(out, 30, 0.0);
                    pair(out, 11, g.p[2] * k);
                    pair(out, 21, g.p[3] * k);
                    pair(out, 31, 0.0);
                    break;
                case sketch::GeoKind::Circle:
                    pair(out, 0, std::string("CIRCLE"));
                    pair(out, 8, layer);
                    pair(out, 10, g.p[0] * k);
                    pair(out, 20, g.p[1] * k);
                    pair(out, 30, 0.0);
                    pair(out, 40, g.p[2] * k);
                    break;
                case sketch::GeoKind::Arc:
                    pair(out, 0, std::string("ARC"));
                    pair(out, 8, layer);
                    pair(out, 10, g.p[0] * k);
                    pair(out, 20, g.p[1] * k);
                    pair(out, 30, 0.0);
                    pair(out, 40, g.p[2] * k);
                    // Back to DEGREES. Our angles are radians; the importer converts the other way,
                    // and a mismatch here would be invisible until an arc came back rotated.
                    pair(out, 50, g.p[3] * kRadToDeg);
                    pair(out, 51, g.p[4] * kRadToDeg);
                    break;
                case sketch::GeoKind::Point:
                    pair(out, 0, std::string("POINT"));
                    pair(out, 8, layer);
                    pair(out, 10, g.p[0] * k);
                    pair(out, 20, g.p[1] * k);
                    pair(out, 30, 0.0);
                    break;
            }
            ++written;
        }

        pair(out, 0, std::string("ENDSEC"));
        pair(out, 0, std::string("EOF"));
        out.flush();
        if (!out) {
            return Error{ErrorCode::Internal, "Writing the DXF failed part way through.",
                         path.string()};
        }
        if (written == 0) {
            // A DXF with no entities is legal and useless. Refusing beats handing back a file that
            // looks like a successful export of nothing.
            std::filesystem::remove(temp, ignored);
            return Error{ErrorCode::InvalidInput, "This sketch has no geometry to export."};
        }
    }

    std::error_code ec;
    std::filesystem::rename(temp, path, ec);
    if (ec) {
        std::filesystem::remove(temp, ignored);
        return Error{ErrorCode::Internal, "The DXF could not be saved to that location.",
                     ec.message()};
    }
    return {};
}

}  // namespace cad::io
