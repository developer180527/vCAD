#include "cad/sketch/Dxf.h"

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

    dimeInput input;
    if (!input.setFile(path.string().c_str())) {
        return Error{ErrorCode::InvalidInput, "That DXF file could not be opened.", path.string()};
    }

    dimeModel model;
    if (!model.read(&input)) {
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

    if (ctx.report.imported() == 0) {
        return Error{ErrorCode::InvalidInput,
                     "That DXF contains no geometry we can import.",
                     ctx.report.summary()};
    }
    return sketch;
}

}  // namespace cad::io
