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

SketchDrawing::Context Controller::drawingContext() const {
    // Assembled per call rather than cached: every field can change between two clicks — the user
    // can zoom, switch units, or edit a different sketch — and a stale copy is the bug this shape
    // exists to make impossible.
    SketchDrawing::Context ctx;
    // const_cast: the drawing MUTATES the sketch on a click and only reads it when measuring,
    // and one Context serves both. The alternative was two nearly identical structs differing
    // in one qualifier, which buys nothing a reader would thank us for.
    ctx.sketch = const_cast<sketch::Sketch*>(activeSketch());
    ctx.worldPerPixel = camera_.worldPerPixel(viewport_);
    ctx.displayUnits = preferences_.displayUnits;
    return ctx;
}

void Controller::setOrbitMode(bool on) {
    if (orbitMode_ == on) return;
    orbitMode_ = on;
    notifyView();
}

void Controller::setSketchTool(SketchTool tool) {
    if (drawing_.tool() == tool) return;
    drawing_.setTool(tool);
    pushSketchOverlay();
    notifyView();
}

Controller::PreviewMeasure Controller::sketchPreviewMeasure() const {
    // Valid while a dimension is being edited, for the same reason as the text above: the field is
    // shown on this and would otherwise vanish the moment the tool that needs it becomes active.
    if (editingDimension_ && editing_.has_value()) {
        const auto& constraints = editing_->constraints();
        if (*editingDimension_ < constraints.size()) {
            return PreviewMeasure{true, false, constraints[*editingDimension_].value, 0.0};
        }
    }
    const auto measure = drawing_.measure();
    return PreviewMeasure{measure.valid, measure.circle, measure.length, measure.angle};
}

Controller::PreviewText Controller::sketchPreviewText() const {
    // A dimension being edited owns the readout. The drawing preview is not running then -- there
    // is no chain and no rubber band -- so without this the field would hide itself and the user
    // would type into nothing visible.
    if (editingDimension_ && editing_.has_value()) {
        const auto& constraints = editing_->constraints();
        if (*editingDimension_ < constraints.size()) {
            const std::string current = units::format(
                units::millimetres(constraints[*editingDimension_].value),
                preferences_.displayUnits);
            return PreviewText{true, dimensionInput_.empty() ? current : dimensionInput_, ""};
        }
    }
    const auto text = drawing_.text(preferences_.displayUnits);
    return PreviewText{text.valid, text.length, text.angle};
}

bool Controller::typeSketchDimension(char c) {
    // A dimension being edited takes the keys. Both paths accept the same characters, because both
    // end at units::parseLength -- "40", "40mm" and "1.5in" have to mean the same thing whether the
    // number sizes a segment being drawn or one drawn ten minutes ago.
    if (editingDimension_) {
        const bool digit = c >= '0' && c <= '9';
        const bool separator = c == '.' || c == ',';
        const bool unit = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        if (!digit && !separator && !unit) return false;
        dimensionInput_.push_back(c);
        notifyView();
        return true;
    }
    if (!drawing_.type(c)) return false;
    notifyView();
    return true;
}

void Controller::backspaceSketchDimension() {
    if (editingDimension_) {
        if (dimensionInput_.empty()) return;
        dimensionInput_.pop_back();
        notifyView();
        return;
    }
    if (drawing_.input().empty()) return;
    drawing_.backspace();
    notifyView();
}

void Controller::clearSketchDimension() {
    if (editingDimension_) {
        // Escape ends the EDIT, leaving the dimension itself in place at the size it was created
        // with. Removing it would make Escape destructive, and the user asked for a dimension.
        editingDimension_.reset();
        dimensionInput_.clear();
        notifyView();
        return;
    }
    if (drawing_.input().empty()) return;
    drawing_.clearInput();
    notifyView();
}

bool Controller::lockSketchDimension() {
    if (!drawing_.lock(drawingContext())) {
        status("That is not a length this document can use.");
        return false;
    }
    status("Length locked — aim the direction, then click");
    pushSketchOverlay();
    notifyView();
    return true;
}

bool Controller::commitSketchDimension() {
    if (!editing_.has_value()) return false;

    if (editingDimension_) {
        // Enter on an empty field means "leave it as it is", not "set it to nothing".
        if (dimensionInput_.empty()) {
            editingDimension_.reset();
            return true;
        }
        const auto parsed = units::parseLength(dimensionInput_, preferences_.displayUnits);
        if (!parsed) {
            status("That is not a length.");
            return false;
        }
        const bool applied = setSketchDimension(*editingDimension_, parsed.value().base());
        editingDimension_.reset();
        dimensionInput_.clear();
        notifyView();
        return applied;
    }

    const auto outcome = drawing_.commitTyped(drawingContext());
    if (!outcome.status.empty()) status(outcome.status);
    if (!outcome.used) return false;
    if (outcome.geometryChanged) {
        lastSketchSolve_ = editing_->solve();
        status(lastSketchSolve_.message);
        notifyDocument();
    }
    pushSketchOverlay();
    notifyView();
    return true;
}

bool Controller::sketchHoverAt(float x, float y) {
    if (environment_ != Environment::Sketch || !drawing_.pending()) return false;
    const auto point = sketchPointAt(x, y);
    if (!point) return false;
    if (!drawing_.hover(drawingContext(), *point)) return false;
    pushSketchOverlay();
    return true;
}

void Controller::endSketchChain() {
    if (!drawing_.pending() && drawing_.input().empty() && !drawing_.lockedLength()) return;
    drawing_.endChain();
    pushSketchOverlay();
    notifyView();
}

void Controller::clearSketch() {
    pushSketchUndo();
    if (!editing_.has_value()) return;
    drawing_.clear(drawingContext());
    pushSketchOverlay();
    notifyDocument();
}
bool Controller::sketchClickAt(float x, float y, bool additive) {
    if (environment_ != Environment::Sketch || !editing_.has_value()) return false;

    // SELECT picks sketch geometry, which nothing in the viewport could do before: the tool existed,
    // the selection existed, and only the 2D canvas ever filled it. Without this there is no way to
    // choose the two lines a constraint applies to, so the constraint menu had nothing to act on.
    if (drawing_.tool() == SketchTool::Select) {
        const auto target = sketchGeometryAt(x, y, kSketchPickRadiusPixels);
        if (!target) {
            // Empty space clears, as it does for the model selection — and for the same reason:
            // the alternative is a selection the user cannot get rid of.
            if (!additive && !sketchSelection_.empty()) {
                clearSketchSelection();
                return true;
            }
            return false;
        }
        selectSketchGeometry(*target, additive);
        return true;
    }

    // Trim is routed out BEFORE the drawing tools, because it is not one: it consumes a click and
    // produces no geometry, and `SketchDrawing` has nothing to contribute to it. Keeping it here
    // rather than inside the drawing state machine also keeps that class about drawing, which is
    // what makes its chain and lock logic readable.
    if (drawing_.tool() == SketchTool::Trim) return trimSketchAt(x, y);
    if (drawing_.tool() == SketchTool::Dimension) return dimensionSketchAt(x, y);

    const auto point = sketchPointAt(x, y);
    // Refused rather than snapped to something arbitrary. A click that missed the plane — edge-on,
    // or a grazing angle — has no sketch coordinate, and inventing one puts geometry where the user
    // cannot see it.
    if (!point) {
        status("That click did not land on the sketch plane.");
        return false;
    }

    // Snapshotted BEFORE the edit, and only when there is a sketch to snapshot. A click that
    // merely starts a chain changes no geometry — the state it would restore is the state you are
    // already in — so the step is dropped again below if nothing happened.
    const std::size_t undoDepth = sketchUndo_.size();
    pushSketchUndo();

    const auto outcome = drawing_.click(drawingContext(), *point);
    if (!outcome.used) {
        if (sketchUndo_.size() > undoDepth) sketchUndo_.pop_back();
        return false;
    }
    if (!outcome.geometryChanged && sketchUndo_.size() > undoDepth) {
        // Nothing to undo: an undo step that restores an identical sketch reads as undo being
        // broken, because the user presses it and sees no change.
        sketchUndo_.pop_back();
    }
    if (!outcome.status.empty()) status(outcome.status);
    if (outcome.geometryChanged) {
        // Every mutation is followed by a solve, because a sketch that does not follow its
        // constraints while you draw is not a sketch — it is a drawing that will jump later.
        lastSketchSolve_ = editing_->solve();
        status(lastSketchSolve_.message);
        notifyDocument();
    }
    pushSketchOverlay();
    notifyView();
    return true;
}

bool Controller::strokeIsCurved(std::span<const std::array<float, 2>> devicePoints) const {
    if (environment_ != Environment::Sketch || devicePoints.size() < 3) return false;

    std::vector<SketchDrawing::Point> onPlane;
    onPlane.reserve(devicePoints.size());
    for (const auto& p : devicePoints) {
        if (const auto point = sketchPointAt(p[0], p[1])) onPlane.push_back(*point);
    }
    if (onPlane.size() < 3) return false;

    // The same eight pixels the stroke path calls straight, converted the same way. A second
    // constant here would be a second definition of "the hand wobbled".
    constexpr double kStraightPixels = 8.0;
    const double tolerance = camera_.worldPerPixel(viewport_) * kStraightPixels;
    return fitStroke(onPlane, tolerance).kind == StrokeKind::Arc;
}

void Controller::showStrokeInk(std::span<const std::array<float, 2>> devicePoints) {
    if (environment_ != Environment::Sketch || !editing_.has_value()) return;

    strokeInk_.clear();
    const sketch::Sketch* active = activeSketch();
    if (active != nullptr && devicePoints.size() >= 2) {
        // A LINE LIST: every segment carries both of its endpoints, because the preview buffer is
        // drawn as separate lines. A strip over these points would join this stroke's end to
        // whatever came next.
        std::optional<std::array<double, 2>> previous;
        for (const auto& p : devicePoints) {
            const auto point = sketchPointAt(p[0], p[1]);
            if (!point) {
                previous.reset();   // the pen crossed off the plane; do not bridge the gap
                continue;
            }
            if (previous) {
                for (const auto& end : {*previous, *point}) {
                    const auto world = active->to3d(end[0], end[1]);
                    strokeInk_.insert(strokeInk_.end(),
                                      {static_cast<float>(world[0]), static_cast<float>(world[1]),
                                       static_cast<float>(world[2])});
                }
            }
            previous = *point;
        }
    }
    ++strokeInkRevision_;
    pushSketchOverlay();
    notifyView();
}

bool Controller::sketchStrokeAt(std::span<const std::array<float, 2>> devicePoints) {
    if (environment_ != Environment::Sketch || !editing_.has_value()) return false;

    // Said out loud. The Select tool consumes nothing, so a shell that forgot to choose a drawing
    // tool got silence from every stroke — which reads as the pen not working rather than as a mode
    // the user is in. This cost an afternoon on the iPad, where there is no visible toolbar state
    // to check against.
    if (drawing_.tool() == SketchDrawing::Tool::Select) {
        status("Choose a drawing tool first.");
        return false;
    }

    std::vector<SketchDrawing::Point> onPlane;
    onPlane.reserve(devicePoints.size());
    for (const auto& p : devicePoints) {
        // Dropped, not refused. A stroke that grazes a solid standing in front of the plane still
        // has a beginning and an end the user meant; rejecting the whole thing because of a few
        // interior samples would make drawing over existing geometry impossible.
        if (const auto point = sketchPointAt(p[0], p[1])) onPlane.push_back(*point);
    }
    if (onPlane.size() < 2) {
        status("That stroke did not land on the sketch plane.");
        return false;
    }

    strokeInk_.clear();
    ++strokeInkRevision_;

    const std::size_t undoDepth = sketchUndo_.size();
    pushSketchUndo();

    const auto outcome = drawing_.stroke(drawingContext(), onPlane);
    if (!outcome.used || !outcome.geometryChanged) {
        if (sketchUndo_.size() > undoDepth) sketchUndo_.pop_back();
        if (!outcome.used) return false;
    }
    if (!outcome.status.empty()) status(outcome.status);
    if (outcome.geometryChanged) {
        // The same solve-per-edit rule as a click. A sketch that does not follow its constraints
        // while you draw is not a sketch — it is a drawing that will jump later.
        lastSketchSolve_ = editing_->solve();
        if (!lastSketchSolve_.solved) status(lastSketchSolve_.message);
        notifyDocument();
    }
    pushSketchOverlay();
    notifyView();
    return true;
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
    if (active == nullptr) return out;

    // The dashes are computed in SKETCH coordinates by the drawing; placing them in 3D needs the
    // sketch's frame, which is why the lift happens here.
    for (const auto& point : drawing_.previewSegments(drawingContext())) {
        const auto world = active->to3d(point[0], point[1]);
        out.insert(out.end(), {static_cast<float>(world[0]), static_cast<float>(world[1]),
                               static_cast<float>(world[2])});
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
    auto preview = sketchPreviewVertices();
    // The pen's own trail, on the same channel: both are proposals rather than geometry, and both
    // are drawn dashed and thin for that reason.
    preview.insert(preview.end(), strokeInk_.begin(), strokeInk_.end());
    scene_->setSketchPreview(preview, sketchPreviewRevision() ^ (strokeInkRevision_ * 1099511628211ull));
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

    // cameraChanged, NOT notifyView. Mutating `camera_` changes nothing anyone can see: the scene
    // holds its own copy of the matrices, and until they are pushed the viewport renders from the
    // old ones. This is the same trap Controller::cameraChanged's own comment records for orbiting,
    // and it meant entering a sketch left the view wherever it was -- the alignment was computed,
    // stored, and never reached the screen until the next unrelated camera event pushed it.
    cameraChanged();
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
