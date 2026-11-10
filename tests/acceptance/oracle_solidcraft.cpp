/// SolidCraft's reference parts, replayed through vCAD's document and compared with answers vCAD
/// did not produce.
///
/// # Why
///
/// Every geometry test in this suite until now was written by the people who wrote the geometry,
/// against values they worked out themselves. That checks the code agrees with its author. These 83
/// parts come from another project (tests/oracle/solidcraft/README.md): a recipe of sketches and
/// features, and the volume, area, centre of mass, bounding box, body count and face count the
/// result must have -- worked out in closed form, or computed by running FreeCAD as a separate
/// program. vCAD, OCCT and this file had no say in any of them.
///
/// # How a case ends
///
/// - PASS: replayed, and every measured property within tolerance.
/// - FAIL: replayed, and something differs -- or a feature vCAD supports failed to compute.
/// - SKIP: the recipe uses an operation or an option this replayer does not translate yet, NAMED.
///   A skip is a statement about this file's vocabulary, not about the part; the summary lists
///   every reason so the gaps are a list, not a mystery.
///
/// Translation is deliberately literal: a recipe feature becomes the document features the
/// application itself would create (Sketch, Extrude, Cut, Fillet... with the same property names
/// Controller uses). Where vCAD's model differs, the translation is the geometric equivalent and
/// says so: a profile with holes is the outer loop extruded and each inner loop cut away, because a
/// vCAD sketch is one wire and one face.
///
/// # How it cannot pass vacuously
///
/// A floor on the number of passing cases, so a replayer that starts skipping everything fails;
/// and a list of known failures that must each still fail, so a fixed case is noticed and the
/// entry removed rather than left to hide the next regression in the same part.

#include "cad/document/Document.h"
#include "cad/kernel/Measurement.h"
#include "cad/kernel/Shape.h"
#include "cad/recompute/Engine.h"
#include "cad/runtime/DocumentRuntime.h"
#include "cad/sketch/Sketch.h"
#include "cad/units/Units.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <numbers>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using namespace cad;
using json = nlohmann::json;
using Vec3 = std::array<double, 3>;

namespace {

// ── what a case's outcome can be ───────────────────────────────────────────────────────────

/// The recipe asks for something this file does not translate. Thrown, not returned, because it
/// can arise anywhere in a replay and always ends it.
struct Unsupported : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/// vCAD tried and failed: a feature did not compute, or a selection found nothing.
struct ReplayFailed : std::runtime_error {
    using std::runtime_error::runtime_error;
};

Vec3 vec(const json& j) { return {j.at(0).get<double>(), j.at(1).get<double>(), j.at(2).get<double>()}; }
double dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Vec3 plus(const Vec3& a, const Vec3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
Vec3 scale(const Vec3& a, double s) { return {a[0] * s, a[1] * s, a[2] * s}; }

// ── a sketch plane, recipe frame to vCAD frame ─────────────────────────────────────────────

/// A recipe sketch plane mapped onto one of vCAD's three origin planes.
///
/// The recipe states its frame explicitly (origin, x_dir, y_dir, normal), and it is not always
/// vCAD's: SolidCraft's XZ has y_dir = -Z where vCAD's v axis is +Z. So nothing is assumed --
/// points go recipe 2D -> world -> vCAD 2D, which is right for any frame lying in the plane.
struct PlaneMap {
    sketch::Plane plane = sketch::Plane::XY;
    std::int64_t index = 0;  ///< what Extrude's "plane" property stores
    Vec3 origin{}, xDir{}, yDir{}, normal{};
    Vec3 vcadAxis{};         ///< the direction an Extrude on this plane grows, for a positive distance

    Vec3 world(double x, double y) const { return plus(origin, plus(scale(xDir, x), scale(yDir, y))); }

    std::array<double, 2> local(const Vec3& w) const {
        switch (plane) {
            case sketch::Plane::XY: return {w[0], w[1]};
            case sketch::Plane::XZ: return {w[0], w[2]};
            case sketch::Plane::YZ: return {w[1], w[2]};
        }
        return {w[0], w[1]};
    }
    std::array<double, 2> local(double x, double y) const { return local(world(x, y)); }
};

PlaneMap planeOf(const json& plane) {
    PlaneMap m;
    m.origin = vec(plane.at("origin"));
    m.xDir = vec(plane.at("x_dir"));
    m.yDir = vec(plane.at("y_dir"));
    m.normal = vec(plane.at("normal"));
    // The normal names the origin plane; the origin must lie ON it. A sketch on an offset plane or
    // on a model face has no global vCAD plane to land on, and placing it on one would build the
    // part somewhere else -- silently, with every number after it wrong.
    const std::pair<sketch::Plane, Vec3> candidates[] = {
        {sketch::Plane::XY, {0, 0, 1}}, {sketch::Plane::XZ, {0, 1, 0}}, {sketch::Plane::YZ, {1, 0, 0}}};
    for (std::int64_t i = 0; i < 3; ++i) {
        const auto& [p, axis] = candidates[i];
        if (std::fabs(std::fabs(dot(m.normal, axis)) - 1.0) < 1e-9) {
            if (std::fabs(dot(m.origin, axis)) > 1e-9) {
                throw Unsupported("sketch on an offset plane or a face (" + plane.value("ref", std::string{}) + ")");
            }
            m.plane = p;
            m.index = i;
            m.vcadAxis = axis;
            return m;
        }
    }
    throw Unsupported("sketch on a plane that is not parallel to an origin plane");
}

// ── sketches ───────────────────────────────────────────────────────────────────────────────

/// One curve of a profile in vCAD's frame, before it is added: the endpoints are what chaining needs.
struct Curve {
    enum class Kind { Line, Circle, Arc } kind = Kind::Line;
    std::array<double, 5> p{};             ///< line: x1 y1 x2 y2; circle: cx cy r; arc: cx cy r a0 a1
    std::array<double, 2> start{}, end{};  ///< equal for a circle
};

/// A recipe curve, in vCAD's frame.
Curve curveOf(const json& e, const PlaneMap& map) {
    const auto at = [&](const json& p) {
        const json& xy = p.contains("at") ? p.at("at") : p;
        return map.local(xy.at(0).get<double>(), xy.at(1).get<double>());
    };
    const std::string type = e.at("type");
    Curve c;
    if (type == "line") {
        c.start = at(e.at("start"));
        c.end = at(e.at("end"));
        c.p = {c.start[0], c.start[1], c.end[0], c.end[1], 0};
        return c;
    }
    if (type == "circle") {
        c.kind = Curve::Kind::Circle;
        const auto ctr = at(e.at("center"));
        c.p = {ctr[0], ctr[1], e.at("radius").get<double>(), 0, 0};
        return c;
    }
    if (type != "arc_three_point" && type != "arc_center_start_sweep") throw Unsupported("sketch entity '" + type + "'");

    // Both arc forms become start / through / end in vCAD's frame, then a centre and a SIGNED sweep:
    // vCAD builds an arc from its start angle to its end angle through the mid-angle, so a signed
    // sweep states direction and major-or-minor in one number, whatever handedness the recipe's
    // frame has relative to vCAD's.
    std::array<double, 2> s{}, m{}, f{};
    if (type == "arc_three_point") {
        s = at(e.at("start"));
        m = at(e.at("mid"));
        f = at(e.at("end"));
    } else {
        const json& cj = e.at("center").contains("at") ? e.at("center").at("at") : e.at("center");
        const json& sj = e.at("start").contains("at") ? e.at("start").at("at") : e.at("start");
        const double cx = cj.at(0), cy = cj.at(1), sx = sj.at(0), sy = sj.at(1);
        const double sweep = e.at("sweep_deg").get<double>() * std::numbers::pi / 180.0;
        const auto rot = [&](double t) {
            const double dx = sx - cx, dy = sy - cy;
            return map.local(cx + dx * std::cos(t) - dy * std::sin(t), cy + dx * std::sin(t) + dy * std::cos(t));
        };
        s = rot(0);
        m = rot(sweep / 2);
        f = rot(sweep);
    }
    const double ax = s[0], ay = s[1], bx = m[0], by = m[1], qx = f[0], qy = f[1];
    const double d = 2 * (ax * (by - qy) + bx * (qy - ay) + qx * (ay - by));
    if (std::fabs(d) < 1e-12) throw ReplayFailed("an arc with collinear points");
    const double ux = ((ax * ax + ay * ay) * (by - qy) + (bx * bx + by * by) * (qy - ay) + (qx * qx + qy * qy) * (ay - by)) / d;
    const double uy = ((ax * ax + ay * ay) * (qx - bx) + (bx * bx + by * by) * (ax - qx) + (qx * qx + qy * qy) * (bx - ax)) / d;
    const double a0 = std::atan2(ay - uy, ax - ux);
    const auto ccw = [](double from, double to) {  // the CCW angle from -> to, in [0, 2pi)
        const double t = std::fmod(to - from, 2 * std::numbers::pi);
        return t < 0 ? t + 2 * std::numbers::pi : t;
    };
    const double toMid = ccw(a0, std::atan2(by - uy, bx - ux));
    const double toEnd = ccw(a0, std::atan2(qy - uy, qx - ux));
    // The mid point is on the CCW arc start->end exactly when it comes before the end.
    const double sweep = toMid < toEnd ? toEnd : toEnd - 2 * std::numbers::pi;
    c.kind = Curve::Kind::Arc;
    c.p = {ux, uy, std::hypot(ax - ux, ay - uy), a0, a0 + sweep};
    c.start = s;
    c.end = f;
    return c;
}

/// Builds a vCAD sketch from the listed curves of a recipe sketch, and nothing else.
///
/// Only the curves a profile names: a recipe sketch often holds more than one loop, and a vCAD
/// sketch turns ALL its curves into one wire, so each loop becomes its own sketch.
std::string buildSketch(const json& recipeSketch, const PlaneMap& map, const std::vector<std::string>& ids) {
    if (!recipeSketch.value("constraints", json::array()).empty() ||
        !recipeSketch.value("dimensions", json::array()).empty()) {
        // The positions in a constrained recipe are where the curves were DRAWN; the part is what
        // the solver makes of them. Replaying the drawn positions would measure the wrong part.
        throw Unsupported("sketch constraints and dimensions");
    }
    std::vector<Curve> curves;
    for (const std::string& id : ids) {
        const auto& entities = recipeSketch.at("entities");
        const auto found = std::find_if(entities.begin(), entities.end(),
                                        [&](const json& e) { return e.value("id", std::string{}) == id; });
        if (found == entities.end()) throw ReplayFailed("a profile names curve '" + id + "', not in the sketch");
        curves.push_back(curveOf(*found, map));
    }

    // Added as a CHAIN, each curve touching the one before: a vCAD sketch adds its curves to one
    // wire in sequence, and a curve touching nothing added so far cannot join it. Neither the
    // profile's list nor the drawing order is a chain in general -- 33 draws both arcs first.
    const auto near = [](const std::array<double, 2>& a, const std::array<double, 2>& b) {
        return std::hypot(a[0] - b[0], a[1] - b[1]) < 1e-6;
    };
    std::vector<Curve> chain;
    if (!curves.empty()) {
        chain.push_back(curves.front());
        curves.erase(curves.begin());
        while (!curves.empty()) {
            const auto next = std::find_if(curves.begin(), curves.end(), [&](const Curve& c) {
                return std::any_of(chain.begin(), chain.end(), [&](const Curve& d) {
                    return near(c.start, d.start) || near(c.start, d.end) || near(c.end, d.start) || near(c.end, d.end);
                });
            });
            // A loop that does not connect is the sketch's problem to report, in its own words.
            chain.push_back(next == curves.end() ? curves.front() : *next);
            curves.erase(next == curves.end() ? curves.begin() : next);
        }
    }

    sketch::Sketch out(map.plane);
    for (const Curve& c : chain) {
        switch (c.kind) {
            case Curve::Kind::Line: out.addLine(c.p[0], c.p[1], c.p[2], c.p[3]); break;
            case Curve::Kind::Circle: out.addCircle(c.p[0], c.p[1], c.p[2]); break;
            case Curve::Kind::Arc: out.addArc(c.p[0], c.p[1], c.p[2], c.p[3], c.p[4]); break;
        }
    }
    return out.serialize();
}

// ── the replay ─────────────────────────────────────────────────────────────────────────────

class Replay {
public:
    explicit Replay(const json& recipe, bool trace = false) : trace_(trace) {
        if (!recipe.value("parameters", json::array()).empty()) throw Unsupported("user parameters");
        for (const json& f : recipe.at("features")) {
            const std::string op = f.at("op");
            if (op == "sketch") sketches_[f.at("name")] = f;
            else if (op == "extrude") extrude(f);
            else if (op == "revolve") revolve(f);
            else if (op == "fillet") edgeFeature(f, "Fillet", "radius", "radius");
            else if (op == "chamfer") edgeFeature(f, "Chamfer", "distance", "distance");
            else if (op == "combine") combine(f);
            else throw Unsupported("operation '" + op + "'");
        }
    }

    /// The bodies the part ends with.
    std::vector<kernel::Shape> bodies() const {
        std::vector<kernel::Shape> out;
        for (const auto& [name, id] : bodies_) out.push_back(shapeOf(id));
        return out;
    }

private:
    document::ObjectId add(const std::string& type, std::vector<std::pair<std::string, document::PropertyValue>> props) {
        auto [next, id] = doc_.add(type);
        auto object = *next.find(id);
        for (auto& [name, value] : props) object = object.withProperty(name, std::move(value));
        doc_ = next.replace(std::make_shared<const document::ObjectData>(std::move(object)));
        recompute::Engine engine(runtime_.registry(), runtime_.cache());
        auto result = engine.recompute(doc_);
        if (!result) throw ReplayFailed(type + ": " + result.error().message);
        doc_ = std::move(result.value().first);
        const auto made = doc_.find(id);
        if (!made || made->output() == nullptr) {
            const auto& error = made ? made->error() : kernel::Error{};
            throw ReplayFailed(type + " did not compute: " + error.message +
                               (error.detail.empty() ? "" : " (" + error.detail + ")"));
        }
        if (trace_) {
            // CAD_ORACLE_TRACE=<case>: every document feature as it is made, to find the step where a
            // replay leaves the expected part.
            const auto m = kernel::measure(made->output()->shape);
            const auto b = kernel::boundingBox(made->output()->shape);
            std::printf("        trace %-10s volume %-12.6g box x %.4g..%.4g y %.4g..%.4g z %.4g..%.4g\n", type.c_str(),
                        m ? m.value().volume : -1.0, b ? b.value().min[0] : 0.0, b ? b.value().max[0] : 0.0,
                        b ? b.value().min[1] : 0.0, b ? b.value().max[1] : 0.0, b ? b.value().min[2] : 0.0,
                        b ? b.value().max[2] : 0.0);
        }
        return id;
    }

    const recompute::Output& outputOf(document::ObjectId id) const {
        const auto o = doc_.find(id);
        if (!o || o->output() == nullptr) throw ReplayFailed("a body has no geometry");
        return *o->output();
    }
    kernel::Shape shapeOf(document::ObjectId id) const { return outputOf(id).shape; }

    document::ObjectId body(const std::string& name) const {
        const auto it = bodies_.find(name);
        if (it == bodies_.end()) throw ReplayFailed("no body named '" + name + "'");
        return it->second;
    }

    /// One closed loop as its own Sketch feature, and the plane it was mapped to.
    std::pair<document::ObjectId, PlaneMap> loopSketch(const json& sketchRecipe, const json& loop) {
        const PlaneMap map = planeOf(sketchRecipe.at("plane"));
        const auto curves = loop.at("curves").get<std::vector<std::string>>();
        const std::string text = buildSketch(sketchRecipe, map, curves);
        const auto id = add("Sketch", {{"sketch", text}, {"plane", map.index}});
        return {id, map};
    }

    /// The distance an Extrude stores so it grows `millimetres` along the RECIPE's normal: vCAD's
    /// plane grows along its own axis, which may be the recipe normal's opposite.
    static double along(const PlaneMap& map, double millimetres) { return millimetres * dot(map.normal, map.vcadAxis); }

    /// A body from a profile: extruded (or revolved) solids for its outer loops, inner loops cut away.
    template <class Make>
    document::ObjectId solidFromProfile(const json& profile, Make make) {
        const json& sketchRecipe = sketches_.at(profile.at("sketch").get<std::string>());
        std::optional<document::ObjectId> solid;
        std::vector<document::ObjectId> holes;
        for (const json& loop : profile.at("loops")) {
            auto [sketchId, map] = loopSketch(sketchRecipe, loop);
            const auto piece = make(sketchId, map);
            if (loop.value("outer", true)) {
                solid = solid ? add("Fuse", {{"a_base", *solid}, {"b_tool", piece}}) : piece;
            } else {
                holes.push_back(piece);
            }
        }
        if (!solid) throw ReplayFailed("a profile with no outer loop");
        for (const auto hole : holes) solid = add("Cut", {{"a_base", *solid}, {"b_tool", hole}});
        return *solid;
    }

    /// Applies a feature's `operation` to the solid it built.
    void apply(const json& f, document::ObjectId tool) {
        const std::string operation = f.value("operation", std::string("new"));
        const auto names = f.value("result_bodies", std::vector<std::string>{});
        if (operation == "new") {
            bodies_[names.empty() ? f.at("name").get<std::string>() : names.front()] = tool;
            return;
        }
        // join and cut act on the body the result is named after; a recipe with one body left may
        // name it differently, and then the one body is the only possible target.
        std::string target = names.empty() ? std::string{} : names.front();
        if (!bodies_.count(target)) {
            if (bodies_.size() != 1) throw ReplayFailed("cannot tell which body '" + operation + "' acts on");
            target = bodies_.begin()->first;
        }
        const char* type = operation == "join" ? "Fuse" : operation == "cut" ? "Cut" : nullptr;
        if (type == nullptr) throw Unsupported(f.at("op").get<std::string>() + " operation '" + operation + "'");
        const auto result = add(type, {{"a_base", body(target)}, {"b_tool", tool}});
        bodies_.erase(target);
        bodies_[names.empty() ? target : names.front()] = result;
    }

    void extrude(const json& f) {
        if (f.contains("thin")) throw Unsupported("thin extrude");
        if (!f.contains("profiles") || f.at("profiles").size() != 1) throw Unsupported("extrude of several profiles");
        const json& extent = f.at("extent");
        if (extent.contains("taper_deg")) throw Unsupported("tapered extrude");
        const std::string type = extent.at("type");

        // What to grow along the recipe normal: a length, and an offset that moves the start back.
        double length = 0, back = 0;
        const double sign = extent.value("direction", std::string("positive")) == "negative" ? -1.0 : 1.0;
        if (type == "distance") {
            length = sign * extent.at("distance").get<double>();
        } else if (type == "through_all") {
            // Longer than any reference part: through-all means "out the far side", and a tool that
            // passes well beyond the part removes exactly what an unbounded one would.
            length = sign * 1000.0;
        } else if (type == "symmetric") {
            if (extent.value("is_full_length", false)) throw Unsupported("symmetric extrude given as full length");
            const double d = extent.at("distance").get<double>();
            length = 2 * d;
            back = d;
        } else if (type == "two_sides") {
            const double d1 = extent.at("side1").get<double>(), d2 = extent.at("side2_opposite").get<double>();
            length = d1 + d2;
            back = d2;
        } else {
            throw Unsupported("extrude extent '" + type + "'");
        }

        const auto solid = solidFromProfile(f.at("profiles").at(0), [&](document::ObjectId sketchId, const PlaneMap& map) {
            auto made = add("Extrude", {{"a_profile", sketchId},
                                        {"distance", units::millimetres(along(map, length))},
                                        {"plane", map.index}});
            if (back != 0) {
                // A vCAD Extrude starts at its sketch, so a symmetric or two-sided one is the full
                // length moved back along the normal.
                const Vec3 shift = scale(map.normal, -back);
                made = add("Translate", {{"a_base", made},
                                         {"dx", units::millimetres(shift[0])},
                                         {"dy", units::millimetres(shift[1])},
                                         {"dz", units::millimetres(shift[2])}});
            }
            return made;
        });
        apply(f, solid);
    }

    void revolve(const json& f) {
        const double degrees = f.at("angle").get<double>();
        if (std::fabs(degrees - 360.0) > 1e-9) {
            // vCAD revolves about a sketch EDGE, whose direction is the edge's own: a partial angle
            // would turn whichever way that edge happens to run.
            throw Unsupported("revolve by less than a full turn");
        }
        const Vec3 o = vec(f.at("axis").at("origin")), dir = vec(f.at("axis").at("dir"));
        const auto solid = solidFromProfile(f.at("profile"), [&](document::ObjectId sketchId, const PlaneMap&) {
            // The axis must be one of the profile's own edges: find the edge lying on the axis line.
            const auto& out = outputOf(sketchId);
            const Vec3 p0 = o, p1 = plus(o, scale(dir, 1.0));
            for (const auto& name : out.map.allNames()) {
                const auto edge = out.map.resolve(name);
                if (!edge || edge->type() != kernel::ShapeType::Edge) continue;
                const auto line = kernel::lineOf(*edge);
                if (!line) continue;
                // Both axis points on the edge's infinite LINE: the edge is the axis, however long.
                const auto onLine = [&](const Vec3& p) {
                    const auto& l = line.value();
                    const Vec3 a{l.origin[0], l.origin[1], l.origin[2]}, u{l.direction[0], l.direction[1], l.direction[2]};
                    const Vec3 ap{p[0] - a[0], p[1] - a[1], p[2] - a[2]};
                    const double t = dot(ap, u);
                    const Vec3 q{ap[0] - u[0] * t, ap[1] - u[1] * t, ap[2] - u[2] * t};
                    return std::sqrt(dot(q, q)) < 1e-7;
                };
                if (onLine(p0) && onLine(p1)) {
                    return add("Revolve", {{"a_profile", sketchId},
                                           {"axis", name},
                                           {"angle", units::Angle::fromBase(2 * std::numbers::pi)}});
                }
            }
            throw Unsupported("revolve about an axis that is not an edge of the profile");
        });
        apply(f, solid);
    }

    /// The edges nearest the recipe's points, on whichever body they are on.
    void edgeFeature(const json& f, const char* type, const char* recipeSize, const char* property) {
        const std::string kind = f.value("type", std::string{});
        if (kind != "constant_radius" && kind != "equal_distance") throw Unsupported(std::string(type) + " of type '" + kind + "'");
        const auto points = f.at("edges");
        if (points.empty()) throw ReplayFailed("no edges listed");

        // Which body: the one with an edge at the first point.
        std::string owner;
        double best = 1e300;
        const Vec3 first = vec(points.at(0).at("point"));
        for (const auto& [name, id] : bodies_) {
            for (const auto& edge : shapeOf(id).subShapes(kernel::ShapeType::Edge)) {
                const auto d = kernel::distanceToPoint(edge, first[0], first[1], first[2]);
                if (d && d.value() < best) best = d.value(), owner = name;
            }
        }
        if (owner.empty() || best > 1e-3) throw ReplayFailed("no edge at the first selected point");

        const auto& out = outputOf(body(owner));
        std::vector<naming::ElementName> picked;
        std::set<std::string> seen;
        for (const json& e : points) {
            const Vec3 p = vec(e.at("point"));
            std::optional<naming::ElementName> nearest;
            double d = 1e300;
            for (const auto& name : out.map.allNames()) {
                const auto shape = out.map.resolve(name);
                if (!shape || shape->type() != kernel::ShapeType::Edge) continue;
                const auto m = kernel::distanceToPoint(*shape, p[0], p[1], p[2]);
                if (m && m.value() < d) d = m.value(), nearest = name;
            }
            if (!nearest || d > 1e-3) throw ReplayFailed("no edge at a selected point");
            if (seen.insert(nearest->toString()).second) picked.push_back(*nearest);
        }
        const auto made = add(type, {{"a_base", body(owner)},
                                     {property, units::millimetres(f.at(recipeSize).get<double>())},
                                     {"edges", picked}});
        bodies_[owner] = made;
    }

    void combine(const json& f) {
        const std::string operation = f.at("operation");
        const char* type = operation == "join" ? "Fuse" : operation == "cut" ? "Cut" : operation == "intersect" ? "Common" : nullptr;
        if (type == nullptr) throw Unsupported("combine '" + operation + "'");
        if (f.value("keep_tools", false)) throw Unsupported("combine keeping its tools");
        const std::string target = f.at("target_body");
        auto result = body(target);
        for (const json& tool : f.at("tool_bodies")) {
            result = add(type, {{"a_base", result}, {"b_tool", body(tool)}});
            bodies_.erase(tool.get<std::string>());
        }
        bodies_[target] = result;
    }

    bool trace_ = false;
    runtime::DocumentRuntime runtime_;
    document::Document doc_;
    std::map<std::string, json> sketches_;
    std::map<std::string, document::ObjectId> bodies_;  ///< the bodies alive now, by recipe name
};

// ── measuring and comparing ────────────────────────────────────────────────────────────────

struct Measured {
    std::size_t bodies = 0, faces = 0;
    double volume = 0, area = 0;
    Vec3 com{};
    kernel::BoundingBox box{};
};

Measured measureAll(const std::vector<kernel::Shape>& shapes) {
    Measured m;
    bool first = true;
    Vec3 moment{};
    for (const auto& shape : shapes) {
        m.bodies += shape.subShapes(kernel::ShapeType::Solid).size();
        m.faces += shape.subShapes(kernel::ShapeType::Face).size();
        const auto props = kernel::measure(shape);
        if (!props) throw ReplayFailed("could not measure a body: " + props.error().message);
        m.volume += props.value().volume;
        m.area += props.value().area;
        for (std::size_t i = 0; i < 3; ++i) moment[i] += props.value().centre[i] * props.value().volume;
        const auto box = kernel::boundingBox(shape);
        if (!box) throw ReplayFailed("could not bound a body: " + box.error().message);
        for (std::size_t i = 0; i < 3; ++i) {
            m.box.min[i] = first ? box.value().min[i] : std::min(m.box.min[i], box.value().min[i]);
            m.box.max[i] = first ? box.value().max[i] : std::max(m.box.max[i], box.value().max[i]);
        }
        first = false;
    }
    if (m.volume != 0) m.com = scale(moment, 1.0 / m.volume);
    return m;
}

/// What differs, one line each; empty when the case matches. Tolerances are SolidCraft's own
/// (docs/oracle.md there): 1e-3 relative for volume and area, 1e-3 of the part's size for the
/// centre of mass and the box, exact counts.
std::vector<std::string> compare(const Measured& got, const json& want) {
    std::vector<std::string> diffs;
    char line[256];
    if (got.bodies != want.at("body_count").get<std::size_t>()) {
        std::snprintf(line, sizeof line, "bodies %zu, expected %zu", got.bodies, want.at("body_count").get<std::size_t>());
        diffs.emplace_back(line);
    }
    const auto relative = [&](const char* what, double g, double w) {
        if (std::fabs(g - w) > 1e-3 * std::max(1.0, std::fabs(w))) {
            std::snprintf(line, sizeof line, "%s %.6g, expected %.6g (%.3g%%)", what, g, w, 100 * (g - w) / w);
            diffs.emplace_back(line);
        }
    };
    relative("volume", got.volume, want.at("volume_mm3"));
    relative("area", got.area, want.at("area_mm2"));

    const Vec3 lo = vec(want.at("bbox").at("min")), hi = vec(want.at("bbox").at("max"));
    const double size = std::sqrt(dot({hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]}, {hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]}));
    const double tol = 1e-3 * std::max(1.0, size);
    const Vec3 com = vec(want.at("center_of_mass"));
    for (std::size_t i = 0; i < 3; ++i) {
        const auto off = [&](const char* what, double g, double w) {
            if (std::fabs(g - w) > tol) {
                std::snprintf(line, sizeof line, "%s[%c] %.6g, expected %.6g", what, "xyz"[i], g, w);
                diffs.emplace_back(line);
            }
        };
        off("centre of mass", got.com[i], com[i]);
        off("box min", got.box.min[i], lo[i]);
        off("box max", got.box.max[i], hi[i]);
    }
    if (want.contains("faces") && got.faces != want.at("faces").get<std::size_t>()) {
        std::snprintf(line, sizeof line, "faces %zu, expected %zu", got.faces, want.at("faces").get<std::size_t>());
        diffs.emplace_back(line);
    }
    return diffs;
}

// ── the cases that are known to fail, and why ──────────────────────────────────────────────

/// Each a case that replays and does not match, with what is understood about it. An entry that
/// starts passing FAILS the test: remove it, so the case guards against the next regression.
const std::map<std::string, std::string> kKnownFailures = {
    // Volume, area, centre of mass and box all match; 11 faces where 9 are expected. vCAD's booleans
    // do not merge coplanar faces afterwards -- Mirror and Pattern do (Features.cpp, mergedFaces) --
    // so the box's base and the cylinder's are two faces, and the cylinder wall is split where it
    // crossed the box. Merging after every boolean renames faces in saved documents, which makes it
    // a naming decision (ADR 0005), not a test fix.
    {"13-combine-join", "booleans leave coplanar faces unmerged"},
};

/// Fewer than this passing means the replayer broke, not that vCAD did.
// 17 when this was written. Raise it as the replayer learns operations; never lower it to make a
// run pass.
constexpr std::size_t kMinimumPassing = 17;

}  // namespace

TEST_CASE("SolidCraft's reference parts replay to the expected geometry", "[oracle][solidcraft]") {
    const std::filesystem::path root = CAD_ORACLE_DIR;
    REQUIRE(std::filesystem::is_directory(root));

    std::vector<std::filesystem::path> cases;
    for (const auto& entry : std::filesystem::directory_iterator(root))
        if (entry.is_directory() && std::filesystem::exists(entry.path() / "recipe.json")) cases.push_back(entry.path());
    std::sort(cases.begin(), cases.end());
    // 83 at the commit README.md names. A partial copy would otherwise pass on what is left.
    REQUIRE(cases.size() >= 83);

    std::size_t passed = 0;
    std::map<std::string, std::vector<std::string>> skippedFor;
    std::vector<std::string> unexpected;

    for (const auto& dir : cases) {
        const std::string name = dir.filename().string();
        const json recipe = json::parse(std::ifstream(dir / "recipe.json"));
        const json want = json::parse(std::ifstream(dir / "expected.json"));

        std::string verdict;
        std::vector<std::string> diffs;
        try {
            const char* traced = std::getenv("CAD_ORACLE_TRACE");
            const Replay replay(recipe, traced != nullptr && name == traced);
            diffs = compare(measureAll(replay.bodies()), want);
            verdict = diffs.empty() ? "PASS" : "FAIL";
        } catch (const Unsupported& u) {
            skippedFor[u.what()].push_back(name);
            std::printf("  SKIP  %-38s %s\n", name.c_str(), u.what());
            continue;
        } catch (const std::exception& e) {
            verdict = "FAIL";
            diffs = {e.what()};
        }

        const bool known = kKnownFailures.count(name) != 0;
        std::printf("  %s  %-38s %s\n", verdict.c_str(), name.c_str(), diffs.empty() ? "" : diffs.front().c_str());
        for (std::size_t i = 1; i < diffs.size(); ++i) std::printf("        %-38s %s\n", "", diffs[i].c_str());
        if (verdict == "PASS") {
            ++passed;
            if (known) unexpected.push_back(name + ": passes now -- remove it from kKnownFailures");
        } else if (!known) {
            unexpected.push_back(name + ": " + diffs.front());
        }
    }

    std::printf("\n  %zu passed, %zu known failures, %zu cases skipped:\n", passed, kKnownFailures.size(),
                [&] { std::size_t n = 0; for (const auto& [_, v] : skippedFor) n += v.size(); return n; }());
    for (const auto& [reason, names] : skippedFor) std::printf("    %3zu  %s\n", names.size(), reason.c_str());

    for (const auto& line : unexpected) {
        INFO(line);
        CHECK(false);
    }
    CHECK(passed >= kMinimumPassing);
}
