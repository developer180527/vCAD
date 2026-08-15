#include "cad/sketch/Dxf.h"

#include <cctype>
#include <cstdlib>
#include <fstream>

#include "cad/log/Log.h"

#include <dime/Input.h>
#include <dime/Model.h>
#include <dime/entities/Arc.h>
#include <dime/entities/Circle.h>
#include <dime/entities/Entity.h>
#include <dime/entities/LWPolyline.h>
#include <dime/entities/Line.h>
#include <dime/entities/Point.h>
#include <dime/entities/Polyline.h>
#include <dime/entities/Vertex.h>
#include <dime/util/Linear.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <numbers>
#include <string>

namespace cad::io {
namespace {

using kernel::Error;
using kernel::ErrorCode;
using sketch::GeoId;
using sketch::Plane;
using sketch::Sketch;

constexpr double kDegToRad = std::numbers::pi / 180.0;
constexpr double kTiny = 1e-12;

bool equalsIgnoringCase(const std::string& a, const std::string& b) {
    return a.size() == b.size()
           && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
                  return std::tolower(static_cast<unsigned char>(x))
                         == std::tolower(static_cast<unsigned char>(y));
              });
}

/// Everything the traversal callback needs. dime's callback takes a void*, so this is what it
/// points at.
struct Context {
    Sketch* sketch = nullptr;
    DxfImportOptions options;
    DxfImportReport report;
    std::map<std::string, std::size_t> unsupported;
};

/// Projects a DXF 3D coordinate onto the sketch plane.
///
/// DXF coordinates are always 3D even in a 2D drawing. Rather than rejecting anything with a
/// non-zero out-of-plane component — which would refuse most real files, since exporters leave
/// stray Z values everywhere — we project and RECORD that we did. A silent projection would turn a
/// slightly-3D drawing into a subtly wrong flat one with no indication.
std::pair<double, double> project(const dimeVec3f& v, Context& ctx) {
    const double scale = ctx.options.scale;
    double u = 0.0;
    double w = 0.0;
    double out = 0.0;
    switch (ctx.options.plane) {
        case Plane::XY: u = v.x; w = v.y; out = v.z; break;
        case Plane::XZ: u = v.x; w = v.z; out = v.y; break;
        case Plane::YZ: u = v.y; w = v.z; out = v.x; break;
    }
    if (std::abs(out) > 1e-9) ctx.report.projected = true;
    return {u * scale, w * scale};
}

bool onConstructionLayer(const dimeEntity* entity, const DxfImportOptions& options) {
    const char* layer = entity->getLayerName();
    if (layer == nullptr) return false;
    const std::string name(layer);
    return std::any_of(options.constructionLayers.begin(), options.constructionLayers.end(),
                       [&](const std::string& candidate) {
                           return equalsIgnoringCase(name, candidate);
                       });
}

void addLine(Context& ctx, std::pair<double, double> a, std::pair<double, double> b,
             bool construction) {
    if (ctx.options.dropDegenerate) {
        const double dx = b.first - a.first;
        const double dy = b.second - a.second;
        if (dx * dx + dy * dy < kTiny) {
            ++ctx.report.degenerate;
            return;
        }
    }
    ctx.sketch->addLine(a.first, a.second, b.first, b.second, construction);
    ++ctx.report.lines;
    if (construction) ++ctx.report.construction;
}

bool visit(const dimeState* /*state*/, dimeEntity* entity, void* userdata) {
    auto& ctx = *static_cast<Context*>(userdata);
    const char* rawName = entity->getEntityName();
    const std::string name = rawName != nullptr ? rawName : "";
    const bool construction = onConstructionLayer(entity, ctx.options);

    if (name == "LINE") {
        auto* line = static_cast<dimeLine*>(entity);
        addLine(ctx, project(line->getCoords(0), ctx), project(line->getCoords(1), ctx),
                construction);
    } else if (name == "CIRCLE") {
        auto* circle = static_cast<dimeCircle*>(entity);
        const auto centre = project(circle->getCenter(), ctx);
        const double radius = circle->getRadius() * ctx.options.scale;
        if (ctx.options.dropDegenerate && radius <= kTiny) {
            ++ctx.report.degenerate;
        } else {
            ctx.sketch->addCircle(centre.first, centre.second, radius, construction);
            ++ctx.report.circles;
            if (construction) ++ctx.report.construction;
        }
    } else if (name == "ARC") {
        auto* arc = static_cast<dimeArc*>(entity);
        dimeVec3f c;
        arc->getCenter(c);
        const auto centre = project(c, ctx);
        const double radius = arc->getRadius() * ctx.options.scale;
        if (ctx.options.dropDegenerate && radius <= kTiny) {
            ++ctx.report.degenerate;
        } else {
            // DXF arc angles are DEGREES, counter-clockwise; ours are radians. Getting this wrong
            // produces an arc of plausible size in the wrong place, which is easy to miss.
            ctx.sketch->addArc(centre.first, centre.second, radius,
                               arc->getStartAngle() * kDegToRad, arc->getEndAngle() * kDegToRad,
                               construction);
            ++ctx.report.arcs;
            if (construction) ++ctx.report.construction;
        }
    } else if (name == "POINT") {
        auto* point = static_cast<dimePoint*>(entity);
        const auto p = project(point->getCoords(), ctx);
        ctx.sketch->addPoint(p.first, p.second, construction);
        ++ctx.report.points;
        if (construction) ++ctx.report.construction;
    } else if (name == "LWPOLYLINE") {
        auto* poly = static_cast<dimeLWPolyline*>(entity);
        const int count = poly->getNumVertices();
        const dxfdouble* xs = poly->getXCoords();
        const dxfdouble* ys = poly->getYCoords();
        const dxfdouble* bulges = poly->getBulges();
        if (count >= 2 && xs != nullptr && ys != nullptr) {
            const double scale = ctx.options.scale;
            // Bulges make a polyline segment an arc. dime exposes them, but reconstructing an arc
            // from a bulge needs the chord and the included angle, and getting that subtly wrong is
            // worse than a straight line the user can see is straight. Flattened and COUNTED, so
            // the report can say the profile lost curvature.
            for (int i = 0; i + 1 < count; ++i) {
                // getBulgeS, plural, and it can be null: a polyline with no curved segments
                // stores no bulge array at all rather than an array of zeros.
                if (bulges != nullptr && bulges[i] != 0.0) ++ctx.report.flattenedBulges;
                addLine(ctx, {xs[i] * scale, ys[i] * scale},
                        {xs[i + 1] * scale, ys[i + 1] * scale}, construction);
            }
            // Flag 1 means closed: the last vertex joins the first, and that segment is not stored.
            // Missing it leaves a profile that is one edge short of closing, which toWire() then
            // rejects as open — a confusing failure for a file that really is closed.
            if ((poly->getFlags() & 1) != 0 && count > 2) {
                addLine(ctx, {xs[count - 1] * scale, ys[count - 1] * scale},
                        {xs[0] * scale, ys[0] * scale}, construction);
            }
            ++ctx.report.polylines;
        }
    } else if (name == "POLYLINE") {
        // The older heavyweight polyline: vertices are separate VERTEX entities owned by it.
        auto* poly = static_cast<dimePolyline*>(entity);
        const int count = poly->getNumCoordVertices();
        std::vector<std::pair<double, double>> pts;
        pts.reserve(static_cast<std::size_t>(std::max(count, 0)));
        for (int i = 0; i < count; ++i) {
            if (const dimeVertex* v = poly->getCoordVertex(i)) {
                pts.push_back(project(v->getCoords(), ctx));
            }
        }
        for (std::size_t i = 0; i + 1 < pts.size(); ++i) {
            addLine(ctx, pts[i], pts[i + 1], construction);
        }
        if ((poly->getFlags() & 1) != 0 && pts.size() > 2) {
            addLine(ctx, pts.back(), pts.front(), construction);
        }
        if (!pts.empty()) ++ctx.report.polylines;
    } else if (!name.empty()) {
        // Counted by type, not just skipped. "SPLINE x 4" tells a user whether what is missing
        // mattered; "some entities were skipped" does not.
        ++ctx.unsupported[name];
    }
    return true;   // keep traversing
}

}  // namespace

std::string DxfImportReport::summary() const {
    std::string text = std::to_string(imported()) + " entities";
    if (construction > 0) text += ", " + std::to_string(construction) + " construction";
    if (degenerate > 0) text += ", " + std::to_string(degenerate) + " degenerate dropped";
    if (flattenedBulges > 0) {
        text += ", " + std::to_string(flattenedBulges) + " curved polyline segments flattened";
    }
    if (projected) text += ", projected onto the sketch plane";
    if (!unsupported.empty()) {
        text += ". Not imported:";
        for (const auto& [kind, count] : unsupported) {
            text += ' ' + kind + " x" + std::to_string(count);
        }
    }
    return text;
}

/// Rejects a DXF whose group codes and values are not paired.
///
/// ASCII DXF is a stream of two-line records: a numeric group code, then its value. A file with an
/// odd number of them ends on a code whose value never arrives — which dime dereferences. See the
/// call site for the crash this prevents.
///
/// Counts lines rather than parsing them: this is a structural precondition, not a validation
/// pass, and anything cleverer would be a second DXF parser sitting in front of the first.
/// dime's own `DXF_MAXLINELEN`, checked against it rather than merely described.
///
/// Named locally because the guard below is about a property of the BUG — the length at which
/// readString writes one past its buffer — and a bare `DXF_MAXLINELEN` at the call site would read
/// as a formatting preference rather than as a memory-safety bound.
///
/// But the static_assert is the point. A comment saying "this must match dime" is a comment; if
/// dime's constant ever moves, the number here becomes silently wrong in whichever direction
/// hurts. Smaller in dime and our guard admits records that overflow it again; larger and we
/// reject files that are perfectly valid. Neither shows up as a test failure, because both look
/// like correct behaviour from outside. Compile-time is the only place this can be caught, and
/// dime's header is already included here — the same argument as the golden ABI snapshot: turn the
/// promise into something that breaks the build when it stops being true.
constexpr std::size_t kDimeLineLimit = 4096;
static_assert(kDimeLineLimit == DXF_MAXLINELEN,
              "dime's DXF_MAXLINELEN changed. The length guard in this file exists because "
              "dimeInput::readString writes lineBuf[idx] with idx == DXF_MAXLINELEN, one past the "
              "end. Update kDimeLineLimit to match, and re-run the boundary test in dxf_fuzz.rs.");

kernel::Result<void> dxfLinesArePaired(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return Error{ErrorCode::InvalidInput, "That DXF file could not be opened.", path.string()};
    }

    std::error_code sizeError;
    const auto fileSize = std::filesystem::file_size(path, sizeError);
    const std::uint64_t bytes = sizeError ? 0 : static_cast<std::uint64_t>(fileSize);

    std::uint64_t lines = 0;
    std::string line;
    std::string previous;
    bool checkedFirst = false;
    while (std::getline(in, line)) {
        // A trailing newline leaves a final empty line that is not a record; anything else empty
        // is malformed anyway and dime will say so.
        if (line.empty() || (line.size() == 1 && line[0] == '\r')) continue;
        ++lines;

        // No single record may reach dime's line buffer length.
        //
        // The third and worst thing fuzzing found, and it is a memory-safety bug in the parser
        // rather than a hang or a null dereference. dime's readString is:
        //
        //     char lineBuf[DXF_MAXLINELEN];                                  // 4096
        //     while (get(c) && ... && idx < DXF_MAXLINELEN) lineBuf[idx++] = c;
        //     this->lineBuf[idx] = '\0';                    // idx can BE 4096 -> one past the end
        //
        // A classic off-by-one: the loop stops at 4096 characters, then the terminator is written
        // at index 4096 of a 4096-byte array. The byte lands on the next member of dimeInput --
        // which is how a 1 MB token turns into `assert(!this->binary)` firing, the field having
        // been overwritten. A file from a stranger corrupting adjacent memory is precisely the
        // class of bug that makes importers the CVE source they are.
        //
        // Refused before dime sees it. Real DXF records are short: a coordinate is tens of
        // characters and the longest legitimate value is a layer or block name, so a limit at the
        // parser's own buffer size rejects nothing a real file contains.
        if (line.size() >= kDimeLineLimit) {
            return Error{ErrorCode::InvalidInput,
                         "That DXF file is corrupt — it contains a record too long to be valid.",
                         path.string() + ": record of " + std::to_string(line.size()) +
                             " bytes exceeds the " + std::to_string(kDimeLineLimit) +
                             "-byte limit"};
        }

        // The FIRST record must be a group code, i.e. an integer.
        //
        // The third crash fuzzing found, and the nastiest to attribute: dime decides a file is
        // BINARY DXF from an "AutoCAD Binary DXF" header, and its ASCII reader then trips
        // `assert(!this->binary)` and aborts the process. An assert in a vendored library, reached
        // from a stranger's file.
        //
        // Checked structurally rather than by looking for that header, because the header is the
        // symptom. ASCII DXF is group-code records from its first byte — a comment is code 999,
        // still an integer — so a first record that is not a number is not ASCII DXF, whatever it
        // is instead. That covers binary DXF, DWG renamed to .dxf, and whatever the next one is.
        if (!checkedFirst) {
            checkedFirst = true;
            std::string trimmed = line;
            while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.back()))) {
                trimmed.pop_back();
            }
            std::size_t start = 0;
            while (start < trimmed.size() &&
                   std::isspace(static_cast<unsigned char>(trimmed[start]))) {
                ++start;
            }
            trimmed.erase(0, start);

            char* end = nullptr;
            std::strtol(trimmed.c_str(), &end, 10);
            if (trimmed.empty() || end == trimmed.c_str() || *end != '\0') {
                return Error{ErrorCode::InvalidInput,
                             "That file is not an ASCII DXF. Binary DXF and DWG are not "
                             "supported — convert it to ASCII DXF first.",
                             path.string() + ": first record is not a group code"};
            }
        }

        // A declared count larger than the file could possibly contain.
        //
        // Also found by fuzzing: an 82-byte file declaring an LWPOLYLINE of two billion vertices
        // took 28.7 SECONDS to import. Not a crash — worse in one way, because it looks like the
        // application has frozen and there is nothing to report. Attacker-controlled input must
        // not be able to wedge the UI, and a count field is the classic lever.
        //
        // The bound is deliberately crude: a vertex needs at least a group code line and a value
        // line per coordinate, so a file cannot describe more vertices than it has BYTES. Anything
        // beyond that is impossible rather than merely large, which is the only kind of count this
        // is entitled to refuse — a real file with a genuinely big polyline must still open.
        if (previous == "90" && bytes > 0) {
            char* end = nullptr;
            const long long declared = std::strtoll(line.c_str(), &end, 10);
            if (end != line.c_str() && declared > static_cast<long long>(bytes)) {
                return Error{ErrorCode::InvalidInput,
                             "That DXF file is corrupt — it declares more geometry than the file "
                             "could contain.",
                             path.string() + ": vertex count " + std::to_string(declared) +
                                 " in a file of " + std::to_string(bytes) + " bytes"};
            }
        }
        previous = line;
    }

    if (lines % 2 != 0) {
        return Error{ErrorCode::InvalidInput,
                     "That DXF file is incomplete — it ends part-way through a record. It may "
                     "have been truncated in transfer.",
                     path.string() + ": odd number of group-code lines (" +
                         std::to_string(lines) + ")"};
    }
    return {};
}

kernel::Result<Sketch> importDxf(const std::filesystem::path& path,
                                const DxfImportOptions& options, DxfImportReport* report) {
    if (!std::filesystem::exists(path)) {
        return Error{ErrorCode::InvalidInput, "That file does not exist.", path.string()};
    }
    if (!(options.scale > 0.0) || !std::isfinite(options.scale)) {
        // Caught here rather than producing a sketch of zeros or NaNs, which would fail much later
        // inside the solver with nothing pointing back at the scale.
        return Error{ErrorCode::InvalidInput, "The import scale must be a positive number."};
    }

    // Structural pre-check, BEFORE dime sees the bytes.
    //
    // Found by fuzzing: a file ending in a group code with no value segfaults dime. The smallest
    // case is 23 bytes —
    //
    //     0\nSECTION\n2\nENTITIES\n0\n
    //
    // — where dime reads the entity-start code 0, asks for the entity name, gets nothing at
    // end-of-file, and strcmps a null pointer. A crash from a 23-byte file, on the one surface
    // where the input comes from a stranger, is the most serious class of bug this codebase can
    // have: people email each other part files.
    //
    // Fixed HERE rather than in vendored dime, deliberately. This layer is ours, the guard holds
    // whatever dime does next, and the parser is going to be replaced anyway — a patch carried in
    // a vendored dependency would be lost at that swap, while this survives it.
    //
    // The rule is DXF's own: ASCII DXF is strictly a stream of (group code, value) line pairs, so
    // an odd count means a code whose value never arrived. That single check covers the whole
    // family the fuzzer found — every crashing truncation was this same shape.
    if (auto paired = dxfLinesArePaired(path); !paired) {
        return paired.error();
    }

    dimeInput input;
    if (!input.setFile(path.string().c_str())) {
        return Error{ErrorCode::InvalidInput, "That DXF file could not be opened.", path.string()};
    }

    dimeModel model;
    if (!model.read(&input)) {
        // dime has already printed its own parse diagnostic to stderr by this point. Ours records
        // WHICH file, which its message does not, and puts both in the same log.
        CAD_WARN(log::Category::Io) << "dime could not parse " << path.string();
        // dime returns false for a malformed file. Deliberately not partial: half a profile looks
        // like a complete one and there is no way for the user to tell which half is missing.
        return Error{ErrorCode::InvalidInput,
                     "That file could not be read as DXF. It may be corrupt, or a DWG renamed to "
                     ".dxf — DWG is not supported; convert it to DXF first.",
                     path.string()};
    }

    Context ctx;
    Sketch sketch(options.plane);
    ctx.sketch = &sketch;
    ctx.options = options;

    model.traverseEntities(visit, &ctx);

    for (const auto& [kind, count] : ctx.unsupported) {
        ctx.report.unsupported.emplace_back(kind, count);
    }
    if (report != nullptr) *report = ctx.report;
    CAD_INFO(log::Category::Io) << "imported " << path.filename().string() << ": "
                                << ctx.report.summary();

    if (ctx.report.imported() == 0) {
        return Error{ErrorCode::InvalidInput,
                     "That DXF contains no geometry we can import.",
                     ctx.report.summary()};
    }
    return sketch;
}

}  // namespace cad::io
