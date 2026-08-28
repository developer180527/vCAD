/// Entering, leaving and editing a sketch as a FEATURE: which sketch is open, what is selected
/// inside it, and the constraints applied to that selection.
///
/// Split out of Controller.cpp, which had reached 2574 lines. The class is unchanged --
/// these are the same methods in the same order, moved verbatim into a file named for what
/// they do, so the system can be read one concern at a time.

#include "Internal.h"

#include <limits>

#include "cad/io/Format.h"
#include "cad/kernel/Guard.h"
#include "cad/kernel/Primitives.h"

#include "cad/render/MetalSurface.h"

#include "cad/io/DocumentStore.h"
#include "cad/sketch/Sketch.h"
#include "cad/sketch/Trim.h"

#include "cad/units/Units.h"

#include <sstream>
#include <tuple>

#include <algorithm>
#include <chrono>


namespace cad::app {

namespace {

/// What a constraint needs from the selection: how many curves, and of what sort.
struct Arity {
    std::size_t count = 1;
    bool linesOnly = false;
    bool roundOnly = false;   ///< circle or arc
    const char* wants = "";
};

Arity arityOf(sketch::ConstraintKind kind) {
    switch (kind) {
        case sketch::ConstraintKind::Horizontal:
        case sketch::ConstraintKind::Vertical:
            return {1, true, false, "one line"};
        case sketch::ConstraintKind::Parallel:
        case sketch::ConstraintKind::Perpendicular:
        case sketch::ConstraintKind::EqualLength:
            return {2, true, false, "two lines"};
        case sketch::ConstraintKind::Radius:
            return {1, false, true, "one circle or arc"};
        default:
            return {0, false, false, ""};   // not applicable from geometry selection
    }
}

}  // namespace


bool Controller::editSketch(ObjectId id) {
    const auto object = history_.current().find(id);
    if (!object || object->type() != "Sketch") {
        status("That is not a sketch.");
        return false;
    }
    const auto* text = object->find("sketch");
    const auto* serialized = text != nullptr ? std::get_if<std::string>(text) : nullptr;
    if (serialized == nullptr) {
        status("That sketch has no geometry to edit.");
        return false;
    }
    auto parsed = sketch::Sketch::deserialize(*serialized);
    if (!parsed) {
        status(parsed.error().message);
        return false;
    }

    editing_ = std::move(parsed.value());
    editingId_ = id;

    // A face-placed sketch arrives from the file knowing WHICH face it is on and not WHERE that is.
    // The frame only ever existed inside computeSketch, so editing one without resolving it here
    // would leave the shell unable to turn a click into a sketch coordinate -- on exactly the
    // sketches in-place editing exists for.
    if (!features::resolveSketchFrame(history_.current(), id, *editing_)) {
        // Refused, not fallen back to a global plane. The face is gone or unrecognisable, and
        // moving the user's geometry somewhere else to make the edit "work" is the failure the
        // naming layer exists to prevent.
        editing_.reset();
        status("This sketch is placed on a face that no longer exists.");
        return false;
    }

    environment_ = Environment::Sketch;
    // Slice starts OFF on every sketch: it is a deliberate act, and a part arriving half-missing
    // because the last sketch left it on is alarming rather than helpful.
    slice_ = false;

    // Remembered BEFORE the camera moves, so finishing puts the user back where they were rather
    // than leaving them staring at the sketch plane. Only on the way IN: re-entering a sketch
    // twice must not overwrite the original 3D view with the first sketch's view.
    if (!cameraBeforeSketch_) cameraBeforeSketch_ = camera_;

    // Orbit is a navigation mode and this is the moment the user asked to DRAW. Left on, the first
    // stroke rotates the model instead, which reads as the sketch tools not working -- and the mode
    // is one toggle away when they do want to turn the part around.
    orbitMode_ = false;

    // Rebuilt so the feature being edited stops being drawn: its placement is decided in refresh,
    // and entering the sketch environment is exactly the moment that decision changes. Without this
    // the overlay and the feature draw the same curves until something else happens to refresh.
    refresh();

    alignCameraToSketch();
    pushSketchOverlay();
    lastSketchSolve_ = editing_->solve();
    notifyDocument();
    notifyView();
    status("Editing " + object->label() + " — " + lastSketchSolve_.message);
    return true;
}

void Controller::seedOriginPlanes() {
    // Three datums in every new part, as objects rather than as something the shell draws.
    //
    // They exist so there is something to sketch ON before any geometry exists. Without them Start
    // Sketch has nothing to select and has to guess a plane, which is what it used to do — and a
    // guessed plane is invisible, so the user cannot tell which one they got.
    //
    // Committed as ONE history entry and then treated as the baseline: seeding them must not make
    // a brand-new document look edited, and must not sit on the undo stack as three steps a user
    // can undo into an empty document with no way to get them back.
    static constexpr struct { const char* label; std::int64_t plane; } kPlanes[]{
        {"XY", 0}, {"XZ", 1}, {"YZ", 2}};

    auto next = history_.current();
    for (const auto& p : kPlanes) {
        auto [added, id] = next.add("Plane");
        next = added;
        const auto object = next.find(id);
        auto updated = object->withProperty("plane", p.plane)
                           .withProperty("size", units::millimetres(100.0))
                           .withLabel(std::string(p.label) + " Plane");
        next = next.replace(std::make_shared<const document::ObjectData>(std::move(updated)));
    }
    history_.commit(std::move(next), "Origin planes");
    refresh();
}

ObjectId Controller::beginSketch() {
    // A selected planar face wins. This is the order the user expects and the order every CAD
    // application uses -- pick the surface, then draw on it -- and it is what makes "sketch on the
    // model" reachable at all: before this, Start Sketch always made an XY sketch no matter what
    // was selected, so a face could be picked and then silently ignored.
    for (const ElementSelection& picked : elementSelection_) {
        const auto owner = history_.current().find(picked.object);
        if (!owner || owner->output() == nullptr) continue;
        const auto shape = owner->output()->map.resolve(picked.element);
        if (!shape) continue;
        // Planar only. A cylindrical face has no single plane to draw on, and refusing here with a
        // reason beats accepting and producing a sketch somewhere arbitrary.
        if (!kernel::planeOf(*shape)) {
            // Covers a curved face AND an edge or vertex, which is why it does not say "curved":
            // telling a user who selected an edge that it is curved sends them looking for the
            // wrong problem.
            status("A sketch needs a flat face or a plane. That selection is neither.");
            continue;
        }
        const ObjectId onFace = addSketchOnFace(picked.object, picked.element.toString());
        if (onFace != ObjectId{}) {
            editSketch(onFace);
            return onFace;
        }
    }

    // The face under the LAST CLICK, even if the selection filter was on Body and the click
    // therefore selected the whole part. Clicking a face and pressing Start Sketch is how every CAD
    // application works; requiring the filter to be switched to Face first is a trap, and it is why
    // "it still does not let me choose a face" was reported after the selection path already worked.
    if (lastPicked_) {
        const auto owner = history_.current().find(lastPicked_->object);
        if (owner && owner->output() != nullptr) {
            const auto shape = owner->output()->map.resolve(lastPicked_->element);
            if (shape && kernel::planeOf(*shape)) {
                const ObjectId onFace =
                    addSketchOnFace(lastPicked_->object, lastPicked_->element.toString());
                if (onFace != ObjectId{}) {
                    editSketch(onFace);
                    return onFace;
                }
            }
        }
    }

    // A datum selected in the TREE, which is how a user picks a plane before any geometry exists.
    // The tree selects OBJECTS, not elements, so the element loop above never sees it — and without
    // this, choosing "XZ Plane" and pressing Start Sketch silently produced an XY sketch.
    for (const ObjectId candidate : selection_) {
        const auto object = history_.current().find(candidate);
        if (!object || object->type() != "Plane" || object->output() == nullptr) continue;
        // A datum has exactly one face; find it by measuring rather than by assuming a position in
        // the map, whose order is not defined.
        for (const auto& name : object->output()->map.allNames()) {
            const auto shape = object->output()->map.resolve(name);
            if (!shape || !kernel::planeOf(*shape)) continue;
            const ObjectId onPlane = addSketchOnFace(candidate, name.toString());
            if (onPlane != ObjectId{}) {
                editSketch(onPlane);
                return onPlane;
            }
            break;
        }
    }

    const ObjectId id = addSketch();
    editSketch(id);
    return id;
}

void Controller::finishSketch() {
    if (!editing_.has_value()) return;
    const auto object = history_.current().find(editingId_);
    if (object) {
        auto next = history_.current();
        auto updated = object->withProperty("sketch", editing_->serialize());
        next = next.replace(std::make_shared<const document::ObjectData>(std::move(updated)));
        // Everything downstream of the sketch has to rebuild: that is the whole point of editing it.
        next = recompute::Engine::invalidate(next, editingId_);
        history_.commit(std::move(next), "Edit Sketch");
    }
    editing_.reset();
    environment_ = Environment::Model;
    restoreCameraAfterSketch();
    // Cleared BEFORE the refresh rebuilds the scene: leaving it up would draw the finished sketch
    // twice, once as the overlay and once as the feature's own edges, fighting for the same pixels.
    drawing_.endChain();
    drawing_.setTool(SketchTool::Select);
    pushSketchOverlay();
    refresh();
    status("Finished sketch");
}

void Controller::cancelSketch() {
    editing_.reset();
    environment_ = Environment::Model;
    // Restored on cancel as well as on finish: abandoning a sketch must cost nothing, and putting
    // the view back only when the user commits would punish them for changing their mind.
    restoreCameraAfterSketch();
    drawing_.endChain();
    drawing_.setTool(SketchTool::Select);
    pushSketchOverlay();
    notifyDocument();
    notifyView();
    status("Discarded sketch changes");
}

sketch::Sketch* Controller::activeSketch() noexcept {
    return editing_.has_value() ? &*editing_ : nullptr;
}

const sketch::Sketch* Controller::activeSketch() const noexcept {
    return editing_.has_value() ? &*editing_ : nullptr;
}

std::optional<sketch::GeoId> Controller::sketchGeometryAt(float x, float y,
                                                          float radiusPixels) const {
    const sketch::Sketch* active = activeSketch();
    if (active == nullptr) return std::nullopt;
    const auto point = sketchPointAt(x, y);
    if (!point) return std::nullopt;

    // The tolerance travels with the pointer, not with the model: a radius in pixels converted
    // here, so the same aperture means the same thing at any zoom — the rule SELECTION.md sets out
    // for picking solids, applied to curves.
    // Both operands widened explicitly: the camera works in floats and this in doubles, and an
    // implicit promotion at the boundary is the warning that hides the ones worth reading.
    const double tolerance = static_cast<double>(camera_.worldPerPixel(viewport_))
                             * static_cast<double>(radiusPixels);
    const double px = (*point)[0];
    const double py = (*point)[1];

    std::optional<sketch::GeoId> best;
    double nearest = tolerance;
    const auto& ids = active->ids();
    const auto& geometry = active->geometry();
    for (std::size_t i = 0; i < geometry.size() && i < ids.size(); ++i) {
        const auto& g = geometry[i];
        double distance = std::numeric_limits<double>::max();
        switch (g.kind) {
            case sketch::GeoKind::Point:
                distance = std::hypot(g.p[0] - px, g.p[1] - py);
                break;
            case sketch::GeoKind::Line: {
                // Distance to the SEGMENT, not to the infinite line: the extension of a short line
                // would otherwise be pickable halfway across the sketch.
                const double ax = g.p[0], ay = g.p[1], bx = g.p[2], by = g.p[3];
                const double dx = bx - ax, dy = by - ay;
                const double lengthSq = dx * dx + dy * dy;
                double t = 0.0;
                if (lengthSq > 1e-18) {
                    t = std::clamp(((px - ax) * dx + (py - ay) * dy) / lengthSq, 0.0, 1.0);
                }
                distance = std::hypot(px - (ax + dx * t), py - (ay + dy * t));
                break;
            }
            case sketch::GeoKind::Circle:
            case sketch::GeoKind::Arc:
                // To the RIM. A circle is picked by its outline, as it is drawn — picking by the
                // centre would make the whole disc a target and swallow anything inside it.
                distance = std::abs(std::hypot(px - g.p[0], py - g.p[1]) - g.p[2]);
                break;
        }
        if (distance <= nearest) {
            nearest = distance;
            best = ids[i];
        }
    }
    return best;
}

std::optional<std::size_t> Controller::dimensionSketchGeometry(sketch::GeoId id) {
    if (!editing_) return std::nullopt;
    const auto* g = editing_->find(id);
    if (g == nullptr) return std::nullopt;

    std::optional<std::size_t> index;
    switch (g->kind) {
        case sketch::GeoKind::Line: {
            const double length = std::hypot(g->p[2] - g->p[0], g->p[3] - g->p[1]);
            if (length < 1e-9) return std::nullopt;
            index = editing_->distance(id, sketch::PointRef::Start, id, sketch::PointRef::End,
                                       length);
            break;
        }
        case sketch::GeoKind::Circle:
        case sketch::GeoKind::Arc:
            if (g->p[2] < 1e-9) return std::nullopt;
            index = editing_->radius(id, g->p[2]);
            break;
        case sketch::GeoKind::Point:
            // A point has no size to dimension. Its POSITION does, and that is LockX/LockY — a
            // different tool, because "where" and "how big" are different questions.
            status("A point has no dimension. Constrain its position instead.");
            return std::nullopt;
    }

    lastSketchSolve_ = editing_->solve();
    // Reported even on success: a dimension that makes a sketch over-constrained solves to a
    // conflict, and the user needs to hear that from the action that caused it.
    status(lastSketchSolve_.message);
    pushSketchOverlay();
    notifyDocument();
    notifyView();
    return index;
}

namespace {

/// How near a click has to be to a sketch curve to count, in DEVICE pixels.
///
/// Eight, matching the aperture solid picking already uses, so a curve and a solid edge are equally
/// easy to hit. A tolerance in pixels rather than millimetres is what makes the tool feel the same
/// at every zoom -- `sketchGeometryAt` converts it through the camera.
constexpr float kSketchPickRadiusPixels = 8.0f;

}  // namespace

std::optional<std::array<float, 2>> Controller::projectToViewport(
    const std::array<double, 3>& world) const {
    const auto& camera = scene_->frame().camera;
    const auto multiply = [](const float m[16], const float v[4], float out[4]) {
        for (int i = 0; i < 4; ++i) {
            out[i] = m[i] * v[0] + m[4 + i] * v[1] + m[8 + i] * v[2] + m[12 + i] * v[3];
        }
    };

    const float point[4]{static_cast<float>(world[0]), static_cast<float>(world[1]),
                         static_cast<float>(world[2]), 1.0f};
    float eye[4];
    float clip[4];
    multiply(camera.view.m, point, eye);
    multiply(camera.projection.m, eye, clip);

    // w comes from the PROJECTION rather than from a guess about the camera mode: orthographic
    // leaves it at 1 and perspective does not, and assuming either is how a viewport ends up
    // correct in one mode and subtly wrong in the other.
    if (!(clip[3] > 1e-6f)) return std::nullopt;

    const float ndcX = clip[0] / clip[3];
    const float ndcY = clip[1] / clip[3];
    return std::array<float, 2>{
        (ndcX * 0.5f + 0.5f) * static_cast<float>(viewport_.width),
        (1.0f - (ndcY * 0.5f + 0.5f)) * static_cast<float>(viewport_.height)};
}

std::vector<Controller::DimensionLabel> Controller::sketchDimensionLabels() const {
    std::vector<DimensionLabel> out;
    const sketch::Sketch* active = activeSketch();
    if (active == nullptr || !active->isPlaced()) return out;

    const auto& constraints = active->constraints();
    for (std::size_t i = 0; i < constraints.size(); ++i) {
        const auto& c = constraints[i];
        const bool radius = c.kind == sketch::ConstraintKind::Radius;
        if (c.kind != sketch::ConstraintKind::Distance && !radius) continue;

        // Where the label belongs, in sketch coordinates.
        std::array<double, 2> anchor{};
        if (radius) {
            const auto* g = active->find(c.a);
            if (g == nullptr) continue;
            anchor = {g->p[0], g->p[1]};   // the centre; the shell offsets in screen pixels
        } else {
            const auto a = active->pointAt(c.a, c.aPoint);
            const auto b = active->pointAt(c.b == sketch::kNoGeo ? c.a : c.b, c.bPoint);
            if (!a || !b) continue;
            anchor = {(a.value()[0] + b.value()[0]) * 0.5,
                      (a.value()[1] + b.value()[1]) * 0.5};
        }

        const auto screen = projectToViewport(active->to3d(anchor[0], anchor[1]));
        if (!screen) continue;   // behind the camera: omitted, because a clamped label points at
                                 // nothing

        DimensionLabel label;
        label.x = (*screen)[0];
        label.y = (*screen)[1];
        label.constraint = i;
        label.radius = radius;
        // Formatted in the DOCUMENT's display units, here rather than in the shell -- the units
        // preference is not a shell concern, and two shells must not disagree about it.
        const std::string value = units::format(units::millimetres(c.value),
                                                preferences_.displayUnits);
        label.text = radius ? "R" + value : value;
        out.push_back(std::move(label));
    }
    return out;
}

bool Controller::dimensionSketchAt(float x, float y) {
    if (!editing_.has_value()) return false;

    const auto target = sketchGeometryAt(x, y, kSketchPickRadiusPixels);
    if (!target) {
        status("Click a curve to dimension it.");
        return false;
    }

    const auto index = dimensionSketchGeometry(*target);
    if (!index) return false;   // it said why -- a point has no size, a zero-length line no length

    // Opened for typing straight away, so the flow is one gesture: click the line, type 40, Enter.
    // Requiring a second click to start editing would make the common case two actions.
    editingDimension_ = *index;
    dimensionInput_.clear();
    notifyView();
    return true;
}

bool Controller::trimSketchAt(float x, float y) {
    if (!editing_.has_value()) return false;

    // Which curve, from the same hit testing selection uses -- so what trim cuts is what a click
    // would have selected, and the two cannot disagree about what is under the pointer.
    const auto target = sketchGeometryAt(x, y, kSketchPickRadiusPixels);
    if (!target) {
        status("Click a curve to trim.");
        return false;
    }
    const auto at = sketchPointAt(x, y);
    if (!at) {
        status("That click did not land on the sketch plane.");
        return false;
    }

    const auto result = sketch::trim(*editing_, *target, *at);
    if (!result) {
        status(result.error().message);
        return false;
    }

    // The trimmed curve may be gone, and a selection naming it would outlive it.
    sketchSelection_.clear();

    lastSketchSolve_ = editing_->solve();

    // What happened, in the order a user cares about: the geometry first, then the constraints,
    // because dropping a constraint is a side effect they did not ask for and would otherwise have
    // to discover by noticing the sketch has more freedom than it did.
    std::string message = result.value().removedWhole  ? "Removed the curve"
                          : result.value().splitInto   ? "Trimmed, splitting the curve in two"
                                                       : "Trimmed";
    if (result.value().constraintsDropped > 0) {
        message += " — dropped " + std::to_string(result.value().constraintsDropped)
                   + (result.value().constraintsDropped == 1 ? " constraint" : " constraints")
                   + " on the ends that moved";
    }
    status(message);

    pushSketchOverlay();
    notifyDocument();
    notifyView();
    return true;
}

bool Controller::setSketchDimension(std::size_t constraint, double millimetres) {
    if (!editing_) return false;
    const auto& constraints = editing_->constraints();
    if (constraint >= constraints.size()) return false;

    // Rejected everywhere: every comparison with NaN is false, so a NaN stored here would reach the
    // solver, whose convergence test is a comparison — and it would report the system SOLVED.
    if (!kernel::isFinite(millimetres)) return false;

    // Validated by KIND, because "positive" is only right for some of them.
    //
    // Distance and Radius are sizes and a non-positive one is not a size. LockX and LockY are
    // POSITIONS, and x = 0 or x = -10 is an ordinary place for a point to be. A single `> 0` guard
    // over both refuses half the values the sketch can legitimately hold — latent while nothing
    // creates a lock through here, and wrong the moment something does.
    switch (constraints[constraint].kind) {
        case sketch::ConstraintKind::Distance:
        case sketch::ConstraintKind::Radius:
            if (!kernel::isPositiveFinite(millimetres)) return false;
            break;
        default:
            // setConstraintValue below refuses any kind that has no value at all, so this does not
            // need to enumerate them — and a new dimension kind gets the finite check for free
            // rather than falling through a `> 0` it may not want.
            break;
    }

    if (!editing_->setConstraintValue(constraint, millimetres)) return false;

    lastSketchSolve_ = editing_->solve();
    status(lastSketchSolve_.message);
    pushSketchOverlay();
    notifyDocument();
    notifyView();
    // The value was applied whatever the solver made of it. A caller that wants to know whether the
    // sketch is still consistent asks the solve report, which is a different question.
    return true;
}

void Controller::selectSketchGeometry(sketch::GeoId id, bool additive) {
    if (!additive) sketchSelection_.clear();
    const auto it = std::find(sketchSelection_.begin(), sketchSelection_.end(), id);
    if (it != sketchSelection_.end()) {
        sketchSelection_.erase(it);   // clicking a selected item again deselects, as in the tree
    } else {
        sketchSelection_.push_back(id);
    }
    notifyView();
    notifyDocument();
}

void Controller::clearSketchSelection() {
    if (sketchSelection_.empty()) return;
    sketchSelection_.clear();
    notifyView();
    notifyDocument();
}

void Controller::deleteSketchSelection() {
    if (!editing_.has_value() || sketchSelection_.empty()) return;

    // Rebuild the sketch without the selected geometry rather than mutating in place: Sketch has no
    // remove-geometry operation, and adding one that also had to rewrite every constraint's targets
    // is a bigger change than this is worth today. Ids are PRESERVED on the survivors, so the
    // constraints that remain still point at the right geometry.
    const sketch::Sketch& old = *editing_;
    sketch::Sketch next(old.plane());
    std::vector<sketch::GeoId> kept;
    for (std::size_t i = 0; i < old.geometry().size() && i < old.ids().size(); ++i) {
        const sketch::GeoId id = old.ids()[i];
        if (std::find(sketchSelection_.begin(), sketchSelection_.end(), id)
            != sketchSelection_.end()) {
            continue;
        }
        kept.push_back(id);
    }

    // Serialise, drop the removed lines, and parse back. Deserialise preserves ids, so this keeps
    // every surviving constraint pointing where it did -- which is exactly why the text format was
    // built to preserve them.
    std::string rebuilt = "sketch 1\n";
    rebuilt += std::string("plane ") + sketch::toString(old.plane()) + "\n";
    std::istringstream in(old.serialize());
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("g ", 0) == 0) {
            std::istringstream row(line);
            std::string tag;
            sketch::GeoId id = 0;
            row >> tag >> id;
            if (std::find(kept.begin(), kept.end(), id) == kept.end()) continue;
            rebuilt += line + "\n";
        } else if (line.rfind("c ", 0) == 0) {
            // A constraint survives only if EVERY geometry it names survives.
            std::istringstream row(line);
            std::string tag;
            std::string kind;
            sketch::GeoId a = 0;
            int ap = 0;
            sketch::GeoId b = 0;
            row >> tag >> kind >> a >> ap >> b;
            const bool aKept = std::find(kept.begin(), kept.end(), a) != kept.end();
            const bool bKept = b == sketch::kNoGeo
                               || std::find(kept.begin(), kept.end(), b) != kept.end();
            if (!aKept || !bKept) continue;
            rebuilt += line + "\n";
        }
    }

    auto parsed = sketch::Sketch::deserialize(rebuilt);
    if (!parsed) {
        status(parsed.error().message);
        return;
    }
    const std::size_t removed = sketchSelection_.size();
    editing_ = std::move(parsed.value());
    sketchSelection_.clear();
    lastSketchSolve_ = editing_->solve();
    notifyView();
    notifyDocument();
    status("Deleted " + std::to_string(removed) + (removed == 1 ? " curve" : " curves"));
}

bool Controller::applySketchConstraint(sketch::ConstraintKind kind) {
    if (!editing_.has_value()) return false;
    const Arity arity = arityOf(kind);
    if (arity.count == 0) {
        status("That constraint needs points selected, which is not supported yet.");
        return false;
    }
    if (sketchSelection_.size() != arity.count) {
        status(std::string("Select ") + arity.wants + " first.");
        return false;
    }

    for (const sketch::GeoId id : sketchSelection_) {
        const auto* g = editing_->find(id);
        if (g == nullptr) return false;
        const bool isLine = g->kind == sketch::GeoKind::Line;
        const bool isRound =
            g->kind == sketch::GeoKind::Circle || g->kind == sketch::GeoKind::Arc;
        if ((arity.linesOnly && !isLine) || (arity.roundOnly && !isRound)) {
            status(std::string("That constraint needs ") + arity.wants + ".");
            return false;
        }
    }

    const sketch::GeoId a = sketchSelection_.front();
    const sketch::GeoId b = sketchSelection_.size() > 1 ? sketchSelection_[1] : sketch::kNoGeo;
    switch (kind) {
        case sketch::ConstraintKind::Horizontal:    editing_->horizontal(a); break;
        case sketch::ConstraintKind::Vertical:      editing_->vertical(a); break;
        case sketch::ConstraintKind::Parallel:      editing_->parallel(a, b); break;
        case sketch::ConstraintKind::Perpendicular: editing_->perpendicular(a, b); break;
        case sketch::ConstraintKind::EqualLength:   editing_->equalLength(a, b); break;
        default: return false;
    }

    const auto report = solveSketch();
    // A constraint that makes the sketch unsatisfiable is reported and KEPT, not silently dropped.
    // The solver names which constraints conflict, so the user can remove the one they mean --
    // rolling this one back automatically would hide the fact that the sketch was already close to
    // over-constrained.
    if (!report.conflicting.empty()) {
        status("Added, but the sketch is now over-constrained: " + report.message);
    } else {
        status(std::string(sketch::toString(kind)) + " applied. " + report.message);
    }
    return true;
}

bool Controller::applySketchRadius(double millimetres) {
    if (!editing_.has_value()) return false;
    if (sketchSelection_.size() != 1) {
        status("Select one circle or arc first.");
        return false;
    }
    if (!(millimetres > 0.0)) {
        status("A radius must be greater than zero.");
        return false;
    }
    const sketch::GeoId id = sketchSelection_.front();
    const auto* g = editing_->find(id);
    if (g == nullptr
        || (g->kind != sketch::GeoKind::Circle && g->kind != sketch::GeoKind::Arc)) {
        status("That constraint needs one circle or arc.");
        return false;
    }
    editing_->radius(id, millimetres);
    const auto report = solveSketch();
    status("Radius applied. " + report.message);
    return true;
}

}  // namespace cad::app
