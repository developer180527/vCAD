#include "cad/sketch/Dxf.h"

#include <cctype>
#include <cstdlib>
#include <fstream>

#include "cad/log/Log.h"

#if CAD_HAVE_RUST_PARSE
#include "cad_parse.h"
#endif

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
#include <array>
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

/// One entity as the FILE described it: raw coordinates, no projection, no scaling, no policy.
///
/// This type is the seam. Reading DXF bytes and building a sketch are two jobs, and keeping a
/// neutral value between them means the second one is written once no matter who does the first.
/// That matters right now because there are two readers — the Rust parser and vendored dime — and
/// the whole point of running them against the same acceptance tests is that everything downstream
/// of the bytes is identical code, not merely similar code.
struct RawEntity {
    enum class Kind { Line, Circle, Arc, Point, Polyline };

    Kind kind = Kind::Line;
    std::string layer;
    /// x,y,z triples. Two for a line, one for a circle/arc/point centre, n for a polyline.
    std::vector<std::array<double, 3>> points;
    double radius = 0.0;
    /// DEGREES, as the file stores them. Converted once, in buildSketch.
    double startAngle = 0.0;
    double endAngle = 0.0;
    long long flags = 0;
    /// Per-segment bulges, or empty when no segment curves.
    std::vector<double> bulges;

    /// True when `points` are already the DRAWING's 2D coordinates rather than model-space 3D,
    /// so x and y map straight onto the sketch plane's two axes and z is meaningless.
    ///
    /// This is the LWPOLYLINE case, and the asymmetry with the heavyweight POLYLINE is real rather
    /// than an oversight. An LWPOLYLINE stores 2D vertices plus a single elevation for the whole
    /// entity: its x and y ARE the drawing's two axes, whatever plane the user imports onto.
    /// A POLYLINE stores genuine 3D vertices, which have to be projected. Running both through
    /// projection would collapse every LWPOLYLINE to a line when importing onto XZ or YZ, because
    /// its "y" would be read as an out-of-plane depth it never was.
    bool planar = false;
};

/// What one reader produced, before any of it becomes a sketch.
struct RawDocument {
    std::vector<RawEntity> entities;
    std::map<std::string, std::size_t> unsupported;
    std::size_t malformed = 0;
};

/// Everything the sketch-building pass carries.
struct Context {
    Sketch* sketch = nullptr;
    DxfImportOptions options;
    DxfImportReport report;
};

/// Projects a DXF 3D coordinate onto the sketch plane.
///
/// DXF coordinates are always 3D even in a 2D drawing. Rather than rejecting anything with a
/// non-zero out-of-plane component — which would refuse most real files, since exporters leave
/// stray Z values everywhere — we project and RECORD that we did. A silent projection would turn a
/// slightly-3D drawing into a subtly wrong flat one with no indication.
std::pair<double, double> project(const std::array<double, 3>& v, Context& ctx) {
    const double scale = ctx.options.scale;
    double u = 0.0;
    double w = 0.0;
    double out = 0.0;
    switch (ctx.options.plane) {
        case Plane::XY: u = v[0]; w = v[1]; out = v[2]; break;
        case Plane::XZ: u = v[0]; w = v[2]; out = v[1]; break;
        case Plane::YZ: u = v[1]; w = v[2]; out = v[0]; break;
    }
    if (std::abs(out) > 1e-9) ctx.report.projected = true;
    return {u * scale, w * scale};
}

bool onConstructionLayer(const std::string& name, const DxfImportOptions& options) {
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

/// Turns raw entities into sketch geometry, applying every option and filling the report.
///
/// The whole domain half of the import, and the ONLY copy of it. Both readers produce a
/// RawDocument and stop; scaling, projection, construction-layer matching, degenerate rejection
/// and the counters all happen exactly once, here. That is what makes the acceptance tests a real
/// comparison between the two readers rather than a comparison between two whole importers.
void buildSketch(const RawDocument& raw, Context& ctx) {
    for (const auto& entity : raw.entities) {
        const bool construction = onConstructionLayer(entity.layer, ctx.options);

        // `planar` entities carry the drawing's own 2D coordinates; everything else is model-space
        // 3D that has to be flattened onto the target plane. See RawEntity::planar.
        const auto toPlane = [&](const std::array<double, 3>& p) {
            if (!entity.planar) return project(p, ctx);
            return std::pair<double, double>{p[0] * ctx.options.scale, p[1] * ctx.options.scale};
        };

        switch (entity.kind) {
            case RawEntity::Kind::Line: {
                if (entity.points.size() < 2) {
                    ++ctx.report.malformed;
                    break;
                }
                addLine(ctx, toPlane(entity.points[0]), toPlane(entity.points[1]), construction);
                break;
            }
            case RawEntity::Kind::Circle: {
                if (entity.points.empty()) {
                    ++ctx.report.malformed;
                    break;
                }
                const auto centre = toPlane(entity.points[0]);
                const double radius = entity.radius * ctx.options.scale;
                if (ctx.options.dropDegenerate && radius <= kTiny) {
                    ++ctx.report.degenerate;
                    break;
                }
                ctx.sketch->addCircle(centre.first, centre.second, radius, construction);
                ++ctx.report.circles;
                if (construction) ++ctx.report.construction;
                break;
            }
            case RawEntity::Kind::Arc: {
                if (entity.points.empty()) {
                    ++ctx.report.malformed;
                    break;
                }
                const auto centre = toPlane(entity.points[0]);
                const double radius = entity.radius * ctx.options.scale;
                if (ctx.options.dropDegenerate && radius <= kTiny) {
                    ++ctx.report.degenerate;
                    break;
                }
                // DXF arc angles are DEGREES, counter-clockwise; ours are radians. Getting this
                // wrong produces an arc of plausible size in the wrong place, which is easy to
                // miss. Converted HERE and nowhere else, so a reader cannot convert them twice.
                ctx.sketch->addArc(centre.first, centre.second, radius,
                                   entity.startAngle * kDegToRad, entity.endAngle * kDegToRad,
                                   construction);
                ++ctx.report.arcs;
                if (construction) ++ctx.report.construction;
                break;
            }
            case RawEntity::Kind::Point: {
                if (entity.points.empty()) {
                    ++ctx.report.malformed;
                    break;
                }
                const auto p = toPlane(entity.points[0]);
                ctx.sketch->addPoint(p.first, p.second, construction);
                ++ctx.report.points;
                if (construction) ++ctx.report.construction;
                break;
            }
            case RawEntity::Kind::Polyline: {
                if (entity.points.size() < 2) {
                    if (!entity.points.empty()) ++ctx.report.malformed;
                    break;
                }
                std::vector<std::pair<double, double>> pts;
                pts.reserve(entity.points.size());
                for (const auto& p : entity.points) pts.push_back(toPlane(p));

                // Bulges make a polyline segment an arc. Both readers expose them, but
                // reconstructing an arc from a bulge needs the chord and the included angle, and
                // getting that subtly wrong is worse than a straight line the user can see is
                // straight. Flattened and COUNTED, so the report can say the profile lost
                // curvature.
                for (std::size_t i = 0; i + 1 < pts.size(); ++i) {
                    if (i < entity.bulges.size() && entity.bulges[i] != 0.0) {
                        ++ctx.report.flattenedBulges;
                    }
                    addLine(ctx, pts[i], pts[i + 1], construction);
                }
                // Flag 1 means closed: the last vertex joins the first, and that segment is not
                // stored. Missing it leaves a profile one edge short of closing, which toWire()
                // then rejects as open — a confusing failure for a file that really is closed.
                if ((entity.flags & 1) != 0 && pts.size() > 2) {
                    addLine(ctx, pts.back(), pts.front(), construction);
                }
                ++ctx.report.polylines;
                break;
            }
        }
    }

    ctx.report.malformed += raw.malformed;
    for (const auto& [kind, count] : raw.unsupported) {
        ctx.report.unsupported.emplace_back(kind, count);
    }
}

// --------------------------------------------------------------------------------------------
// Reader: vendored dime
//
// Retained as the fallback for builds without a Rust toolchain, since CAD_REQUIRE_RUST is off by
// default. It now only fills a RawDocument — every crash guard in dxfLinesArePaired below still
// stands in front of it, because this path still hands a stranger's bytes to C.
//
// COMPILED IN BOTH CONFIGURATIONS, deliberately, even though it is only CALLED in one. A fallback
// behind an #if that nobody builds stops compiling within a release or two, and discovers that on
// the machine that has no cargo — which is exactly the machine that needs it. The cost is that
// dime stays linked; the benefit is that the fallback is real.
// --------------------------------------------------------------------------------------------

[[maybe_unused]] std::array<double, 3> triple(const dimeVec3f& v) {
    // dime declares `typedef float dxfdouble`, so these widen. Explicit rather than implicit: the
    // precision floor is dime's and is documented on importDxf, and a silent promotion here would
    // make it look as though the value had ever been a double.
    return {static_cast<double>(v.x), static_cast<double>(v.y), static_cast<double>(v.z)};
}

[[maybe_unused]] bool visit(const dimeState* /*state*/, dimeEntity* entity,
                            void* userdata) {
    auto& raw = *static_cast<RawDocument*>(userdata);
    const char* rawName = entity->getEntityName();
    const std::string name = rawName != nullptr ? rawName : "";
    const char* rawLayer = entity->getLayerName();
    const std::string layer = rawLayer != nullptr ? rawLayer : "";

    if (name == "LINE") {
        auto* line = static_cast<dimeLine*>(entity);
        RawEntity e;
        e.kind = RawEntity::Kind::Line;
        e.layer = layer;
        e.points = {triple(line->getCoords(0)), triple(line->getCoords(1))};
        raw.entities.push_back(std::move(e));
    } else if (name == "CIRCLE") {
        auto* circle = static_cast<dimeCircle*>(entity);
        RawEntity e;
        e.kind = RawEntity::Kind::Circle;
        e.layer = layer;
        e.points = {triple(circle->getCenter())};
        e.radius = static_cast<double>(circle->getRadius());
        raw.entities.push_back(std::move(e));
    } else if (name == "ARC") {
        auto* arc = static_cast<dimeArc*>(entity);
        dimeVec3f c;
        arc->getCenter(c);
        RawEntity e;
        e.kind = RawEntity::Kind::Arc;
        e.layer = layer;
        e.points = {triple(c)};
        e.radius = static_cast<double>(arc->getRadius());
        e.startAngle = static_cast<double>(arc->getStartAngle());
        e.endAngle = static_cast<double>(arc->getEndAngle());
        raw.entities.push_back(std::move(e));
    } else if (name == "POINT") {
        auto* point = static_cast<dimePoint*>(entity);
        RawEntity e;
        e.kind = RawEntity::Kind::Point;
        e.layer = layer;
        e.points = {triple(point->getCoords())};
        raw.entities.push_back(std::move(e));
    } else if (name == "LWPOLYLINE") {
        auto* poly = static_cast<dimeLWPolyline*>(entity);
        const int count = poly->getNumVertices();
        const dxfdouble* xs = poly->getXCoords();
        const dxfdouble* ys = poly->getYCoords();
        // getBulgeS, plural, and it can be null: a polyline with no curved segments stores no
        // bulge array at all rather than an array of zeros.
        const dxfdouble* bulges = poly->getBulges();
        if (count >= 2 && xs != nullptr && ys != nullptr) {
            RawEntity e;
            e.kind = RawEntity::Kind::Polyline;
            e.layer = layer;
            e.flags = poly->getFlags();
            e.planar = true;
            for (int i = 0; i < count; ++i) {
                e.points.push_back({static_cast<double>(xs[i]), static_cast<double>(ys[i]), 0.0});
            }
            if (bulges != nullptr) {
                for (int i = 0; i < count; ++i) {
                    e.bulges.push_back(static_cast<double>(bulges[i]));
                }
            }
            raw.entities.push_back(std::move(e));
        }
    } else if (name == "POLYLINE") {
        // The older heavyweight polyline: vertices are separate VERTEX entities owned by it.
        auto* poly = static_cast<dimePolyline*>(entity);
        const int count = poly->getNumCoordVertices();
        RawEntity e;
        e.kind = RawEntity::Kind::Polyline;
        e.layer = layer;
        e.flags = poly->getFlags();
        for (int i = 0; i < count; ++i) {
            if (const dimeVertex* v = poly->getCoordVertex(i)) {
                e.points.push_back(triple(v->getCoords()));
            }
        }
        if (!e.points.empty()) raw.entities.push_back(std::move(e));
    } else if (!name.empty()) {
        // Counted by type, not just skipped. "SPLINE x 4" tells a user whether what is missing
        // mattered; "some entities were skipped" does not.
        ++raw.unsupported[name];
    }
    return true;   // keep traversing
}

}  // namespace

std::string DxfImportReport::summary() const {
    std::string text = std::to_string(imported()) + " entities";
    if (construction > 0) text += ", " + std::to_string(construction) + " construction";
    if (degenerate > 0) text += ", " + std::to_string(degenerate) + " degenerate dropped";
    if (malformed > 0) text += ", " + std::to_string(malformed) + " unreadable";
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

#if CAD_HAVE_RUST_PARSE
// --------------------------------------------------------------------------------------------
// Reader: the Rust parser
//
// The reason this exists is narrow and worth stating plainly: DXF is the one surface in this
// application where the bytes come from a stranger. People email each other part files. Fuzzing
// the dime path found a segfault from 23 bytes, a 28-second hang from 82 bytes, and a one-past-the
// -end write — three memory-safety or denial-of-service bugs in an afternoon, in code that had
// been working correctly for years on well-formed input.
//
// None of the guards below go away when this path is taken; see the note on dxfLinesArePaired.
// --------------------------------------------------------------------------------------------

/// Reads a NUL-terminated string through one of the buffer-returning accessors.
///
/// The accessors return the FULL length even when they wrote nothing, so the retry is a single
/// resize rather than a doubling loop. Worth doing properly: silently truncating here would move
/// an entity onto a different layer, which changes whether it is construction geometry.
template <class Accessor>
std::string readString(Accessor&& accessor) {
    char stack[128];
    const std::size_t needed = accessor(stack, sizeof(stack));
    if (needed == 0) return {};
    if (needed < sizeof(stack)) return std::string(stack, needed);

    std::string heap(needed + 1, '\0');
    const std::size_t written = accessor(heap.data(), heap.size());
    if (written != needed) return {};
    heap.resize(needed);
    return heap;
}

kernel::Result<RawDocument> readWithRust(const std::filesystem::path& path) {
    int code = CAD_DXF_OK;
    CadDxfDocument* parsed = cad_dxf_parse_file(path.string().c_str(), &code);
    if (parsed == nullptr) {
        const std::string message = readString([&](char* buffer, std::size_t capacity) {
            return cad_dxf_error_message(code, buffer, capacity);
        });
        CAD_WARN(log::Category::Io)
            << "dxf: rust parser rejected " << path.string() << " (code " << code << "): "
            << message;
        return Error{ErrorCode::InvalidInput, message,
                     path.string() + ": parser code " + std::to_string(code)};
    }

    // The handle is released on every path out of here, including the throwing ones the vector
    // growth below can take.
    struct Guard {
        CadDxfDocument* doc;
        ~Guard() { cad_dxf_free(doc); }
    } guard{parsed};

    RawDocument raw;
    const std::size_t count = cad_dxf_entity_count(parsed);
    raw.entities.reserve(count);
    raw.malformed = cad_dxf_malformed_count(parsed);

    for (std::size_t i = 0; i < count; ++i) {
        RawEntity entity;
        entity.layer = readString([&](char* buffer, std::size_t capacity) {
            return cad_dxf_entity_layer(parsed, i, buffer, capacity);
        });
        entity.radius = cad_dxf_entity_radius(parsed, i);
        entity.startAngle = cad_dxf_entity_start_angle(parsed, i);
        entity.endAngle = cad_dxf_entity_end_angle(parsed, i);
        entity.flags = cad_dxf_entity_flags(parsed, i);

        switch (cad_dxf_entity_kind(parsed, i)) {
            case CAD_DXF_LINE: entity.kind = RawEntity::Kind::Line; break;
            case CAD_DXF_CIRCLE: entity.kind = RawEntity::Kind::Circle; break;
            case CAD_DXF_ARC: entity.kind = RawEntity::Kind::Arc; break;
            case CAD_DXF_POINT: entity.kind = RawEntity::Kind::Point; break;
            case CAD_DXF_POLYLINE:
                entity.kind = RawEntity::Kind::Polyline;
                // The parser folds LWPOLYLINE and POLYLINE into one kind, and both arrive as the
                // drawing's 2D coordinates -- it never reads a vertex Z. See RawEntity::planar.
                entity.planar = true;
                break;
            default:
                // An index past the end returns -1, and a kind this build does not know about
                // means the header and the library are different vintages. Either way the entity
                // is not something we can place, and counting it is more useful than guessing.
                ++raw.malformed;
                CAD_WARN(log::Category::Io)
                    << "dxf: entity " << i << " has an unknown kind; header and cad-parse "
                    << "library may be out of step";
                continue;
        }

        const std::size_t points = cad_dxf_entity_point_count(parsed, i);
        entity.points.reserve(points);
        for (std::size_t j = 0; j < points; ++j) {
            double x = 0.0;
            double y = 0.0;
            if (cad_dxf_entity_point(parsed, i, j, &x, &y) == 0) break;
            entity.points.push_back({x, y, 0.0});
        }

        const std::size_t bulges = cad_dxf_entity_bulge_count(parsed, i);
        entity.bulges.reserve(bulges);
        for (std::size_t j = 0; j < bulges; ++j) {
            entity.bulges.push_back(cad_dxf_entity_bulge(parsed, i, j));
        }

        raw.entities.push_back(std::move(entity));
    }

    const std::size_t kinds = cad_dxf_unsupported_count(parsed);
    for (std::size_t i = 0; i < kinds; ++i) {
        std::string name = readString([&](char* buffer, std::size_t capacity) {
            return cad_dxf_unsupported_name(parsed, i, buffer, capacity);
        });
        if (!name.empty()) raw.unsupported[name] = cad_dxf_unsupported_occurrences(parsed, i);
    }

    CAD_DEBUG(log::Category::Io)
        << "dxf: rust parser read " << path.filename().string() << " -- " << raw.entities.size()
        << " entities, " << raw.malformed << " unreadable, " << raw.unsupported.size()
        << " unsupported types";
    return raw;
}
#endif  // CAD_HAVE_RUST_PARSE

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

    RawDocument raw;
#if CAD_HAVE_RUST_PARSE
    // The Rust reader when the build has one. The guards above still run in front of it: they cost
    // one pass over a file that is about to be read anyway, and they are the difference between
    // "this build is safe" and "this build is safe as long as CAD_REQUIRE_RUST was on", which is
    // not a property anyone can check from a crash report.
    {
        auto parsed = readWithRust(path);
        if (!parsed) return parsed.error();
        raw = std::move(parsed.value());
    }
#else
    dimeInput input;
    if (!input.setFile(path.string().c_str())) {
        CAD_WARN(log::Category::Io) << "dxf: could not open " << path.string();
        return Error{ErrorCode::InvalidInput, "That DXF file could not be opened.", path.string()};
    }

    dimeModel model;
    if (!model.read(&input)) {
        // dime has already printed its own parse diagnostic to stderr by this point. Ours records
        // WHICH file, which its message does not, and puts both in the same log.
        CAD_WARN(log::Category::Io) << "dxf: dime could not parse " << path.string();
        // dime returns false for a malformed file. Deliberately not partial: half a profile looks
        // like a complete one and there is no way for the user to tell which half is missing.
        return Error{ErrorCode::InvalidInput,
                     "That file could not be read as DXF. It may be corrupt, or a DWG renamed to "
                     ".dxf — DWG is not supported; convert it to DXF first.",
                     path.string()};
    }

    model.traverseEntities(visit, &raw);
    CAD_DEBUG(log::Category::Io)
        << "dxf: dime read " << path.filename().string() << " -- " << raw.entities.size()
        << " entities, " << raw.unsupported.size() << " unsupported types";
#endif

    Context ctx;
    Sketch sketch(options.plane);
    ctx.sketch = &sketch;
    ctx.options = options;

    buildSketch(raw, ctx);

    if (report != nullptr) *report = ctx.report;

    // Fidelity loss gets its own line at WARNING, not a clause buried in the INFO summary. These
    // are the two things a user comes back to the log about — "why is my profile open" and "why is
    // this the wrong size" — and both are answered by geometry that was in the file and is not in
    // the sketch.
    if (ctx.report.malformed > 0) {
        CAD_WARN(log::Category::Io)
            << "dxf: " << ctx.report.malformed << " entities in " << path.filename().string()
            << " could not be read and were dropped";
    }
    if (ctx.report.flattenedBulges > 0) {
        CAD_WARN(log::Category::Io)
            << "dxf: " << ctx.report.flattenedBulges << " curved polyline segments in "
            << path.filename().string() << " were flattened to straight lines";
    }
    for (const auto& [kind, count] : ctx.report.unsupported) {
        CAD_DEBUG(log::Category::Io)
            << "dxf: not imported from " << path.filename().string() << ": " << kind << " x"
            << count;
    }

    CAD_INFO(log::Category::Io) << "dxf: imported " << path.filename().string() << ": "
                                << ctx.report.summary();

    if (ctx.report.imported() == 0) {
        CAD_WARN(log::Category::Io) << "dxf: " << path.filename().string()
                                    << " contained no importable geometry";
        return Error{ErrorCode::InvalidInput,
                     "That DXF contains no geometry we can import.",
                     ctx.report.summary()};
    }
    return sketch;
}

}  // namespace cad::io
