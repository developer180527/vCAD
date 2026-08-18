/// Entering, leaving and editing a sketch as a FEATURE: which sketch is open, what is selected
/// inside it, and the constraints applied to that selection.
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
    sketchPending_.reset();
    sketchHover_.reset();
    sketchInput_.clear();
    sketchTool_ = SketchTool::Select;
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
    sketchPending_.reset();
    sketchHover_.reset();
    sketchInput_.clear();
    sketchTool_ = SketchTool::Select;
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
