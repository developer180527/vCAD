/// Drawing with the mouse: the tool, the chain, snapping, inferred constraints, live dimensions
/// and everything the viewport draws while a sketch is open.
///
/// This is the file that grew fastest and the reason Controller.cpp reached 2500 lines. It is
/// still Controller's methods -- a mechanical split, not a redesign -- but it is now the one
/// place to read to understand what a click does while sketching.
///
/// Split out of Controller.cpp, which had reached 2574 lines. The class is unchanged --
/// these are the same methods in the same order, moved verbatim into a file named for what
/// they do, so the system can be read one concern at a time.

#include "Internal.h"

#include "cad/io/Format.h"
#include "cad/kernel/Primitives.h"

#include "cad/render/MetalSurface.h"

#include "cad/io/DocumentStore.h"
#include "cad/sketch/Sketch.h"

#include "cad/units/Units.h"

#include <sstream>
#include <tuple>

#include <algorithm>
#include <chrono>


namespace cad::app {

void Controller::setOrbitMode(bool on) {
    if (orbitMode_ == on) return;
    orbitMode_ = on;
    notifyView();
}

void Controller::setSketchTool(SketchTool tool) {
    if (sketchTool_ == tool) return;
    sketchTool_ = tool;
    // Abandoned, not carried across. A line waiting for its second point means nothing to the
    // circle tool, and keeping it is how a stray segment appears from a click made a minute ago.
    sketchPending_.reset();
    sketchHover_.reset();
    sketchInput_.clear();
    notifyView();
}

Controller::PreviewMeasure Controller::sketchPreviewMeasure() const {
    PreviewMeasure out;
    if (!sketchPending_ || !sketchHover_) return out;

    const double dx = (*sketchHover_)[0] - (*sketchPending_)[0];
    const double dy = (*sketchHover_)[1] - (*sketchPending_)[1];
    out.length = std::sqrt(dx * dx + dy * dy);
    out.circle = sketchTool_ == SketchTool::Circle;
    // Degrees from the sketch's own +u axis, not from the world's X. The number has to mean
    // something in the plane the user is drawing on, or it is nonsense on a tilted face.
    out.angle = out.circle ? 0.0 : std::atan2(dy, dx) * 180.0 / std::numbers::pi;
    out.valid = true;
    return out;
}

Controller::PreviewText Controller::sketchPreviewText() const {
    PreviewText out;
    const PreviewMeasure measure = sketchPreviewMeasure();
    if (!measure.valid) return out;

    // What the user TYPED wins, because that is the value that will be used — showing the measured
    // length beside a number being typed to replace it is showing two answers to one question.
    const std::string typed = sketchInput_;
    out.length = typed.empty()
                     ? units::format(units::millimetres(measure.length),
                                     preferences_.displayUnits, 2)
                     : typed;
    if (measure.circle) {
        out.length = "R " + out.length;
    } else {
        out.angle = units::format(units::degrees(measure.angle), 1);
    }
    out.valid = true;
    return out;
}

bool Controller::typeSketchDimension(char c) {
    // Only while something is pending: a digit typed with nothing half-drawn is a shortcut, not a
    // dimension, and swallowing it would make the keyboard feel dead.
    if (!sketchPending_) return false;
    const bool digit = c >= '0' && c <= '9';
    const bool separator = c == '.' || c == ',';
    // Unit letters are accepted so "12mm" and "0.5in" parse; units::parseLength decides what is
    // actually valid, and it is the one place that knows.
    const bool unit = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    if (!digit && !separator && !unit) return false;
    sketchInput_.push_back(c);
    notifyView();
    return true;
}

void Controller::backspaceSketchDimension() {
    if (sketchInput_.empty()) return;
    sketchInput_.pop_back();
    notifyView();
}

void Controller::clearSketchDimension() {
    if (sketchInput_.empty()) return;
    sketchInput_.clear();
    notifyView();
}

bool Controller::commitSketchDimension() {
    if (!sketchPending_ || !sketchHover_ || sketchInput_.empty()) return false;

    auto parsed = units::parseLength(sketchInput_, preferences_.displayUnits);
    if (!parsed) {
        status(parsed.error().message);
        return false;
    }
    const double value = parsed.value().base();
    if (!(value > 0.0)) {
        status("A dimension has to be greater than zero.");
        return false;
    }

    // The DIRECTION comes from the pointer and the SIZE from the number. That is what makes typing
    // feel like drawing: you aim with the mouse and say how far with the keyboard.
    const double dx = (*sketchHover_)[0] - (*sketchPending_)[0];
    const double dy = (*sketchHover_)[1] - (*sketchPending_)[1];
    const double length = std::sqrt(dx * dx + dy * dy);
    if (length < 1e-9) {
        status("Point the cursor in the direction first.");
        return false;
    }
    const std::array<double, 2> at{(*sketchPending_)[0] + dx / length * value,
                                   (*sketchPending_)[1] + dy / length * value};

    const std::array<double, 2> first = *sketchPending_;
    sketchPending_.reset();
    sketchHover_.reset();
    sketchInput_.clear();

    if (!editing_.has_value()) return false;

    if (sketchTool_ == SketchTool::Circle) {
        const auto id = editing_->addCircle(first[0], first[1], value);
        // A DRIVING dimension, not merely geometry that happens to be this size. Without the
        // constraint the number the user typed is forgotten the instant anything else moves —
        // which is the difference between a sketch and a drawing.
        editing_->radius(id, value);
    } else {
        const auto id = editing_->addLine(first[0], first[1], at[0], at[1]);
        editing_->distance(id, sketch::PointRef::Start, id, sketch::PointRef::End, value);
    }

    lastSketchSolve_ = editing_->solve();
    status(lastSketchSolve_.message);
    pushSketchOverlay();
    notifyDocument();
    notifyView();
    return true;
}

bool Controller::sketchHoverAt(float x, float y) {
    // Only while a shape is half-drawn. Before the first click the pointer says nothing about what
    // is being made, and following it anyway would draw a band from the sketch origin to the mouse.
    if (environment_ != Environment::Sketch || !sketchPending_) return false;

    const auto point = sketchPointAt(x, y);
    if (!point) return false;
    if (sketchHover_ && std::abs((*sketchHover_)[0] - (*point)[0]) < 1e-9
        && std::abs((*sketchHover_)[1] - (*point)[1]) < 1e-9) {
        return false;   // no movement worth a repaint
    }
    sketchHover_ = *point;
    pushSketchOverlay();
    return true;
}

void Controller::endSketchChain() {
    if (!sketchPending_ && sketchInput_.empty()) return;
    sketchPending_.reset();
    sketchHover_.reset();
    sketchInput_.clear();
    pushSketchOverlay();
    notifyView();
}

void Controller::clearSketch() {
    if (!editing_.has_value()) return;
    *editing_ = sketch::Sketch(editing_->plane());
    editing_->setPlacement(editing_->placement());
    endSketchChain();
    pushSketchOverlay();
    notifyDocument();
}

std::optional<std::array<double, 2>> Controller::snapSketchPoint(
    const std::array<double, 2>& at) const {
    const sketch::Sketch* active = activeSketch();
    if (active == nullptr) return std::nullopt;

    // A tolerance in PIXELS, converted to sketch units. A fixed millimetre tolerance snaps from
    // across the screen when zoomed out and never snaps at all when zoomed in -- the user's hand is
    // steady in pixels, not in millimetres.
    const double tolerance = camera_.worldPerPixel(viewport_) * kSnapPixels;

    std::optional<std::array<double, 2>> best;
    double nearest = tolerance;
    const auto consider = [&](double u, double v) {
        const double dx = u - at[0];
        const double dy = v - at[1];
        const double distance = std::sqrt(dx * dx + dy * dy);
        if (distance <= nearest) {
            nearest = distance;
            best = std::array<double, 2>{u, v};
        }
    };

    for (const auto& g : active->geometry()) {
        switch (g.kind) {
            case sketch::GeoKind::Line:
                consider(g.p[0], g.p[1]);
                consider(g.p[2], g.p[3]);
                break;
            case sketch::GeoKind::Circle:
            case sketch::GeoKind::Arc:
                consider(g.p[0], g.p[1]);   // the centre
                break;
            case sketch::GeoKind::Point:
                consider(g.p[0], g.p[1]);
                break;
        }
    }
    // The sketch origin, which is what a user aims at when they want a shape anchored.
    consider(0.0, 0.0);
    return best;
}

bool Controller::sketchClickAt(float x, float y) {
    if (environment_ != Environment::Sketch || !editing_.has_value()) return false;
    if (sketchTool_ == SketchTool::Select) return false;

    auto point = sketchPointAt(x, y);
    if (!point) {
        status("That click did not land on the sketch plane.");
        return false;
    }
    // Snapped BEFORE anything else uses it, so the point that starts a segment and the point that
    // ends one are the same point when they should be. Without this a click lands NEAR the previous
    // endpoint, the two segments do not meet, and no amount of careful aiming closes the profile.
    if (const auto snapped = snapSketchPoint(*point)) point = snapped;

    if (!sketchPending_) {
        sketchPending_ = *point;
        sketchHover_ = *point;
        status(sketchTool_ == SketchTool::Line ? "Line: click the next point, Escape to finish"
                                               : "Circle: click to set the radius");
        pushSketchOverlay();
        notifyView();
        return true;
    }

    const std::array<double, 2> first = *sketchPending_;
    const double dx = (*point)[0] - first[0];
    const double dy = (*point)[1] - first[1];
    const double length = std::sqrt(dx * dx + dy * dy);
    if (length < 1e-9) {
        // A second click on the first point is how a user says "done" with the mouse alone.
        endSketchChain();
        return true;
    }

    if (sketchTool_ == SketchTool::Circle) {
        const auto id = editing_->addCircle(first[0], first[1], length);
        sketchPending_.reset();
        sketchHover_.reset();
        sketchInput_.clear();
        (void)id;
    } else {
        const auto id = editing_->addLine(first[0], first[1], (*point)[0], (*point)[1]);
        inferSketchConstraint(id, first, *point);

        // CHAINING: the endpoint becomes the next segment's start. This is the whole behaviour of
        // a CAD line tool -- click, click, click draws a connected run -- and its absence is why a
        // closed rectangle was impossible to draw.
        sketchPending_ = *point;
        sketchHover_ = *point;
        sketchInput_.clear();

        // Back onto a point the chain already used: the loop is closed and the run is over.
        // Checked after the segment is added, so the closing segment itself is drawn.
        if (closesSketchLoop(*point)) endSketchChain();
    }

    lastSketchSolve_ = editing_->solve();
    status(lastSketchSolve_.message);
    pushSketchOverlay();
    notifyDocument();
    notifyView();
    return true;
}

bool Controller::closesSketchLoop(const std::array<double, 2>& at) const {
    const sketch::Sketch* active = activeSketch();
    if (active == nullptr) return false;
    // Exact, because the point has already been snapped: a loop closes when the click landed ON an
    // existing endpoint, not merely near one.
    int touching = 0;
    for (const auto& g : active->geometry()) {
        if (g.kind != sketch::GeoKind::Line) continue;
        if (std::abs(g.p[0] - at[0]) < 1e-9 && std::abs(g.p[1] - at[1]) < 1e-9) ++touching;
        if (std::abs(g.p[2] - at[0]) < 1e-9 && std::abs(g.p[3] - at[1]) < 1e-9) ++touching;
    }
    // Two segments meeting here means the run has come back on itself. One is just the segment
    // that was only this moment drawn.
    return touching >= 2;
}

void Controller::inferSketchConstraint(std::uint32_t id, const std::array<double, 2>& from,
                                       const std::array<double, 2>& to) {
    if (!editing_.has_value()) return;

    // Horizontal and vertical only, for now. They are the two a hand aims at constantly and the two
    // whose absence leaves an otherwise careful sketch under-constrained -- which is the difference
    // between a parametric sketch and a drawing.
    const double dx = std::abs(to[0] - from[0]);
    const double dy = std::abs(to[1] - from[1]);
    const double length = std::sqrt(dx * dx + dy * dy);
    if (length < 1e-9) return;

    // A few degrees of tolerance: a user aiming at horizontal misses by a pixel or two, and a rule
    // that only fires on an exact match never fires at all.
    constexpr double kTolerance = 0.05;   // sin of ~3 degrees
    if (dy / length < kTolerance) {
        editing_->horizontal(id);
    } else if (dx / length < kTolerance) {
        editing_->vertical(id);
    }
}

std::vector<float> Controller::sketchOverlayVertices() const {
    std::vector<float> out;
    const sketch::Sketch* active = activeSketch();
    if (active == nullptr) return out;

    // The sketch's own frame, so the lines land exactly where sketchPointAt says the user clicked.
    // Reusing that mapping rather than repeating it: two copies would drift, and the symptom would
    // be geometry drawn one place and stored another.
    const auto place = [&](double u, double v, std::array<float, 3>& xyz) {
        const auto world = active->to3d(u, v);
        xyz = {static_cast<float>(world[0]), static_cast<float>(world[1]),
               static_cast<float>(world[2])};
    };
    const auto segment = [&](double u1, double v1, double u2, double v2) {
        std::array<float, 3> a{};
        std::array<float, 3> b{};
        place(u1, v1, a);
        place(u2, v2, b);
        out.insert(out.end(), a.begin(), a.end());
        out.insert(out.end(), b.begin(), b.end());
    };

    for (const auto& g : active->geometry()) {
        switch (g.kind) {
            case sketch::GeoKind::Line:
                segment(g.p[0], g.p[1], g.p[2], g.p[3]);
                break;
            case sketch::GeoKind::Circle: {
                // Fixed segment count rather than sag-based: this is an editing overlay redrawn on
                // every stroke, and a circle that changes its own facet count as it is resized
                // shimmers distractingly while you drag.
                constexpr int kSegments = 64;
                for (int i = 0; i < kSegments; ++i) {
                    const double a0 = 2.0 * std::numbers::pi * i / kSegments;
                    const double a1 = 2.0 * std::numbers::pi * (i + 1) / kSegments;
                    segment(g.p[0] + g.p[2] * std::cos(a0), g.p[1] + g.p[2] * std::sin(a0),
                            g.p[0] + g.p[2] * std::cos(a1), g.p[1] + g.p[2] * std::sin(a1));
                }
                break;
            }
            case sketch::GeoKind::Arc: {
                constexpr int kSegments = 32;
                const double span = g.p[4] - g.p[3];
                for (int i = 0; i < kSegments; ++i) {
                    const double a0 = g.p[3] + span * i / kSegments;
                    const double a1 = g.p[3] + span * (i + 1) / kSegments;
                    segment(g.p[0] + g.p[2] * std::cos(a0), g.p[1] + g.p[2] * std::sin(a0),
                            g.p[0] + g.p[2] * std::cos(a1), g.p[1] + g.p[2] * std::sin(a1));
                }
                break;
            }
            case sketch::GeoKind::Point:
                break;   // nothing to draw as a line
        }
    }
    return out;
}

std::vector<float> Controller::sketchPreviewVertices() const {
    std::vector<float> out;
    const sketch::Sketch* active = activeSketch();
    if (active == nullptr || !sketchPending_ || !sketchHover_) return out;

    // DASHED, and dashed in screen terms. A preview must be distinguishable from committed
    // geometry at a glance -- otherwise the user cannot tell what is real -- and a dash measured in
    // world units becomes a solid line when zoomed in and a row of dots when zoomed out, which is
    // when it is least readable.
    const float perPixel = camera_.worldPerPixel(viewport_);
    const double dash = std::max(1e-6f, perPixel) * 6.0;   // ~6 px on, 6 px off
    const auto emit = [&](double u1, double v1, double u2, double v2) {
        const double dx = u2 - u1;
        const double dy = v2 - v1;
        const double length = std::sqrt(dx * dx + dy * dy);
        if (length < 1e-12) return;
        // Cap the count so a wildly zoomed-out view cannot ask for a million tiny segments while
        // the user drags -- past that density the dashes are sub-pixel and read as a line anyway.
        const int steps = static_cast<int>(std::min(2000.0, std::ceil(length / (dash * 2.0))));
        for (int i = 0; i < steps; ++i) {
            const double t0 = std::min(1.0, (i * 2.0 * dash) / length);
            const double t1 = std::min(1.0, (i * 2.0 * dash + dash) / length);
            const auto a3 = active->to3d(u1 + dx * t0, v1 + dy * t0);
            const auto b3 = active->to3d(u1 + dx * t1, v1 + dy * t1);
            out.insert(out.end(), {static_cast<float>(a3[0]), static_cast<float>(a3[1]),
                                   static_cast<float>(a3[2]), static_cast<float>(b3[0]),
                                   static_cast<float>(b3[1]), static_cast<float>(b3[2])});
        }
    };

    if (sketchTool_ == SketchTool::Circle) {
        const double dx = (*sketchHover_)[0] - (*sketchPending_)[0];
        const double dy = (*sketchHover_)[1] - (*sketchPending_)[1];
        const double radius = std::sqrt(dx * dx + dy * dy);
        constexpr int kSegments = 64;
        for (int i = 0; i < kSegments; ++i) {
            const double a0 = 2.0 * std::numbers::pi * i / kSegments;
            const double a1 = 2.0 * std::numbers::pi * (i + 1) / kSegments;
            emit((*sketchPending_)[0] + radius * std::cos(a0),
                 (*sketchPending_)[1] + radius * std::sin(a0),
                 (*sketchPending_)[0] + radius * std::cos(a1),
                 (*sketchPending_)[1] + radius * std::sin(a1));
        }
    } else {
        emit((*sketchPending_)[0], (*sketchPending_)[1], (*sketchHover_)[0], (*sketchHover_)[1]);
    }
    return out;
}

std::uint64_t Controller::sketchPreviewRevision() const {
    // Includes the DASH SIZE by construction, because the vertices are the dashes: a zoom changes
    // them and the buffer is re-sent, which is correct and is the only case where a camera move
    // must re-upload anything.
    std::uint64_t hash = 1469598103934665603ull;
    for (const float f : sketchPreviewVertices()) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &f, sizeof(bits));
        for (int i = 0; i < 4; ++i) {
            hash ^= (bits >> (i * 8)) & 0xffu;
            hash *= 1099511628211ull;
        }
    }
    return hash;
}

Controller::CurveMesh Controller::sketchCurveMesh(double widthPixels) const {
    CurveMesh mesh;
    const sketch::Sketch* active = activeSketch();
    if (active == nullptr) return mesh;

    const auto lines = sketchOverlayVertices();
    if (lines.size() < 6) return mesh;

    // Half-width in world units, so the ribbon is `widthPixels` across on screen whatever the zoom.
    const double half = camera_.worldPerPixel(viewport_) * widthPixels * 0.5;

    // Offset within the SKETCH PLANE rather than towards the camera. A sketch is drawn face-on, so
    // in-plane widening looks right there and merely foreshortens when the view is orbited — which
    // is the correct behaviour anyway: the ribbon is geometry on the plane, not a screen decal.
    std::array<double, 3> planeNormal{0.0, 0.0, 1.0};
    if (const auto& frame = active->resolvedFrame()) {
        const auto n = frame->normal();
        planeNormal = {n[0], n[1], n[2]};
    } else {
        switch (active->plane()) {
            case sketch::Plane::XY: planeNormal = {0, 0, 1}; break;
            case sketch::Plane::XZ: planeNormal = {0, -1, 0}; break;
            case sketch::Plane::YZ: planeNormal = {1, 0, 0}; break;
        }
    }

    for (std::size_t i = 0; i + 5 < lines.size(); i += 6) {
        const std::array<double, 3> a{lines[i], lines[i + 1], lines[i + 2]};
        const std::array<double, 3> b{lines[i + 3], lines[i + 4], lines[i + 5]};
        std::array<double, 3> along{b[0] - a[0], b[1] - a[1], b[2] - a[2]};
        const double length =
            std::sqrt(along[0] * along[0] + along[1] * along[1] + along[2] * along[2]);
        if (length < 1e-12) continue;
        for (double& v : along) v /= length;

        // side = along x normal: perpendicular to the segment, in the plane.
        const std::array<double, 3> side{along[1] * planeNormal[2] - along[2] * planeNormal[1],
                                         along[2] * planeNormal[0] - along[0] * planeNormal[2],
                                         along[0] * planeNormal[1] - along[1] * planeNormal[0]};

        const auto push = [&](const std::array<double, 3>& p, double sign) {
            render::CadVertex vertex{};
            for (int k = 0; k < 3; ++k) {
                vertex.position[k] = static_cast<float>(p[k] + side[k] * half * sign);
                vertex.normal[k] = static_cast<float>(planeNormal[k]);
            }
            vertex.element = 0;
            mesh.vertices.push_back(vertex);
        };

        const auto base = static_cast<std::uint32_t>(mesh.vertices.size());
        push(a, +1.0);
        push(a, -1.0);
        push(b, -1.0);
        push(b, +1.0);
        for (const std::uint32_t offset : {0u, 1u, 2u, 0u, 2u, 3u}) {
            mesh.indices.push_back(base + offset);
        }
    }
    return mesh;
}

std::uint64_t Controller::sketchOverlayRevision() const {
    // FNV-1a over the vertices. A digest rather than a counter, for the same reason the instance
    // buffers use one: an edit that does not change the geometry must not re-upload it, and a
    // counter cannot tell the difference.
    std::uint64_t hash = 1469598103934665603ull;
    for (const float f : sketchOverlayVertices()) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &f, sizeof(bits));
        for (int i = 0; i < 4; ++i) {
            hash ^= (bits >> (i * 8)) & 0xffu;
            hash *= 1099511628211ull;
        }
    }
    return hash;
}

void Controller::pushSketchProfile() {
    if (!scene_) return;
    const sketch::Sketch* active = activeSketch();
    if (active == nullptr) {
        scene_->setSketchProfile({}, {}, 0);
        return;
    }

    // toFace() refusing is the whole signal: an open profile shades nothing. No error is raised
    // and none is wanted -- an open sketch is a perfectly good sketch that is not finished yet.
    auto face = active->toFace();
    if (!face) {
        scene_->setSketchProfile({}, {}, 0);
        return;
    }

    // NAMED before tessellating. The tessellator assigns every vertex an element index and builds
    // the element table from the map, so a bare face with an empty map produces no mesh at all --
    // it fails, silently as far as the viewport is concerned, and the profile simply never appears.
    naming::NamingContext naming(0, 0);
    auto map = naming.nameprimitive(face.value(), {});
    if (!map) {
        scene_->setSketchProfile({}, {}, 0);
        return;
    }

    document::Output out;
    out.shape = face.value();
    out.map = std::move(map.value());
    const auto mesh = render::tessellate(out, render::TessellationSettings{});
    if (!mesh || mesh.value()->vertices.empty()) {
        scene_->setSketchProfile({}, {}, 0);
        return;
    }
    // The overlay's revision covers the same geometry, so reusing it means the profile re-uploads
    // exactly when the curves move and not when the camera does.
    scene_->setSketchProfile(mesh.value()->vertices, mesh.value()->indices,
                             sketchOverlayRevision());
}

void Controller::pushSketchOverlay() {
    if (!scene_) return;
    // Cleared when leaving the sketch, or the finished sketch would be drawn twice: once as the
    // overlay and once as the feature's own edges, at slightly different depths.
    // Ribbons rather than lines: bgfx draws lines one physical pixel wide, which on a Retina
    // display is a hairline. The revision folds in the camera scale, because the ribbon's width is
    // in world units derived from it — a zoom really does change the geometry here.
    // DEVICE pixels, because the viewport is measured in them. 2.5 device pixels is barely one
    // logical pixel on a Retina display, which is the hairline this replaced; 4 gives a sketch line
    // about as heavy as the model's own edges.
    const auto curves = sketchCurveMesh(4.0);
    const std::uint64_t scale =
        static_cast<std::uint64_t>(camera_.worldPerPixel(viewport_) * 1e6);
    scene_->setSketchCurves(curves.vertices, curves.indices, sketchOverlayRevision() ^ scale);

    // The line overlay is kept for the PREVIEW only, which is dashed and thin by design — a
    // proposal should not look as solid as committed geometry.
    scene_->setSketchOverlay({}, 0);
    const auto preview = sketchPreviewVertices();
    scene_->setSketchPreview(preview, sketchPreviewRevision());
    pushSketchProfile();
}

void Controller::restoreCameraAfterSketch() {
    // Slice is a sketch-scoped view state. Left on after the sketch closes, the user is looking at
    // a part with half of it missing and no visible reason why.
    slice_ = false;
    applySlice();
    if (!cameraBeforeSketch_) return;
    camera_ = *cameraBeforeSketch_;
    cameraBeforeSketch_.reset();
    notifyView();
}

void Controller::setSliceEnabled(bool on) {
    if (slice_ == on) return;
    slice_ = on;
    applySlice();
    notifyView();
}

void Controller::applySlice() {
    if (!scene_) return;

    const sketch::Sketch* active = activeSketch();
    if (!slice_ || active == nullptr) {
        scene_->setSectionPlanes({});
        return;
    }

    // The sketch's plane, with its normal turned to face the CAMERA. Which way the frame's own
    // normal points is an accident of how the face was built, and using it directly would cut away
    // the half the user is looking at half the time — the tool would appear to work at random.
    std::array<double, 3> origin{0.0, 0.0, 0.0};
    std::array<double, 3> normal{0.0, 0.0, 1.0};
    if (const auto& frame = active->resolvedFrame()) {
        origin = {frame->origin[0], frame->origin[1], frame->origin[2]};
        const auto n = frame->normal();
        normal = {n[0], n[1], n[2]};
    } else {
        switch (active->plane()) {
            case sketch::Plane::XY: normal = {0, 0, 1}; break;
            case sketch::Plane::XZ: normal = {0, -1, 0}; break;
            case sketch::Plane::YZ: normal = {1, 0, 0}; break;
        }
    }

    const auto basis = camera_.basis();
    const double towardCamera = -(normal[0] * basis.forward[0] + normal[1] * basis.forward[1] +
                                  normal[2] * basis.forward[2]);
    if (towardCamera < 0.0) {
        normal = {-normal[0], -normal[1], -normal[2]};
    }

    render::SectionPlane plane;
    for (int i = 0; i < 3; ++i) plane.normal[i] = static_cast<float>(normal[i]);
    // The offset is the plane's own position along that normal, so the cut lands exactly on the
    // sketch rather than a fixed distance from the world origin.
    plane.offset = static_cast<float>(origin[0] * normal[0] + origin[1] * normal[1] +
                                      origin[2] * normal[2]);
    scene_->setSectionPlanes(std::span<const render::SectionPlane>(&plane, 1));
}

void Controller::alignCameraToSketch() {
    if (!editing_.has_value()) return;

    // Face-on, which is how every CAD application opens a sketch and is most of what "the sketch
    // is in the same world as the model" has to mean: the geometry stays where it is, the camera
    // moves to look at it squarely. Nothing is swapped and nothing is reprojected.
    std::array<float, 3> origin{0.0f, 0.0f, 0.0f};
    std::array<float, 3> normal{0.0f, 0.0f, 1.0f};
    std::array<float, 3> up{0.0f, 1.0f, 0.0f};

    if (const auto& frame = editing_->resolvedFrame()) {
        for (int i = 0; i < 3; ++i) {
            origin[i] = static_cast<float>(frame->origin[i]);
            up[i] = static_cast<float>(frame->v[i]);
        }
        const auto n = frame->normal();
        for (int i = 0; i < 3; ++i) normal[i] = static_cast<float>(n[i]);
    } else {
        switch (editing_->plane()) {
            case sketch::Plane::XY: normal = {0, 0, 1}; up = {0, 1, 0}; break;
            case sketch::Plane::XZ: normal = {0, -1, 0}; up = {0, 0, 1}; break;
            case sketch::Plane::YZ: normal = {1, 0, 0}; up = {0, 0, 1}; break;
        }
    }

    // `up` is the sketch's own v axis, so the sketch's +v points up the screen and what the user
    // draws is oriented the way the sketch's coordinates say it is -- not the way the world's
    // happen to fall.
    camera_.alignTo(origin.data(), normal.data(), up.data());
    notifyView();
}

std::optional<std::array<double, 2>> Controller::sketchPointAt(float x, float y) const {
    const sketch::Sketch* active = activeSketch();
    if (active == nullptr) return std::nullopt;

    // The frame the sketch is actually on. A face-placed sketch that has not been resolved yet has
    // no 3D interpretation of its own coordinates, so there is nothing to map a pixel onto -- the
    // same refusal Sketch::toWire makes, for the same reason.
    std::array<double, 3> origin{0.0, 0.0, 0.0};
    std::array<double, 3> u{1.0, 0.0, 0.0};
    std::array<double, 3> v{0.0, 1.0, 0.0};
    if (const auto& resolved = active->resolvedFrame()) {
        origin = {resolved->origin[0], resolved->origin[1], resolved->origin[2]};
        u = {resolved->u[0], resolved->u[1], resolved->u[2]};
        v = {resolved->v[0], resolved->v[1], resolved->v[2]};
    } else if (active->needsResolution()) {
        return std::nullopt;
    } else {
        switch (active->plane()) {
            case sketch::Plane::XY: u = {1, 0, 0}; v = {0, 1, 0}; break;
            case sketch::Plane::XZ: u = {1, 0, 0}; v = {0, 0, 1}; break;
            case sketch::Plane::YZ: u = {0, 1, 0}; v = {0, 0, 1}; break;
        }
    }

    const std::array<double, 3> normal{u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2],
                                       u[0] * v[1] - u[1] * v[0]};

    const auto ray = camera_.rayThrough(x, y, viewport_);
    const double denominator = normal[0] * ray.direction[0] + normal[1] * ray.direction[1] +
                               normal[2] * ray.direction[2];

    // Edge-on. Not an error and not a fallback: a ray parallel to the plane meets it nowhere, and
    // any number returned here would be a coordinate the user did not point at. 1e-6 rather than
    // zero because a grazing angle produces a finite but meaningless answer -- at 0.0001 the hit
    // is kilometres away, off screen, and looks to the user like the click was ignored anyway.
    if (std::abs(denominator) < 1e-6) return std::nullopt;

    const double t = ((origin[0] - ray.origin[0]) * normal[0] +
                      (origin[1] - ray.origin[1]) * normal[1] +
                      (origin[2] - ray.origin[2]) * normal[2]) / denominator;

    const std::array<double, 3> hit{ray.origin[0] + ray.direction[0] * t,
                                    ray.origin[1] + ray.direction[1] * t,
                                    ray.origin[2] + ray.direction[2] * t};

    // Back into the sketch's own axes. u and v are unit and perpendicular (PlaneFrame and the
    // global planes both guarantee it), so projecting is a dot product rather than a solve.
    const std::array<double, 3> d{hit[0] - origin[0], hit[1] - origin[1], hit[2] - origin[2]};
    return std::array<double, 2>{d[0] * u[0] + d[1] * u[1] + d[2] * u[2],
                                 d[0] * v[0] + d[1] * v[1] + d[2] * v[2]};
}

sketch::SolveReport Controller::solveSketch() {
    if (!editing_.has_value()) return {};
    lastSketchSolve_ = editing_->solve();
    notifyView();
    notifyDocument();
    return lastSketchSolve_;
}

}  // namespace cad::app
