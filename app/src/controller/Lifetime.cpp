/// Construction, notifications, the document tree, selection, properties and history.
///
/// The parts of the controller that are about the DOCUMENT rather than about geometry, a
/// viewport or a tool.
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
Controller::Controller() {
    meshes_ = std::make_unique<render::MeshCache>(blobs_);
    active_ = backend_.handle();
    scene_ = std::make_unique<render::SceneBuilder>(*meshes_, *active_.resources);
    viewport_.width = 1280;
    viewport_.height = 800;
    scene_->setViewport(viewport_);
    registerCommands();
    seedOriginPlanes();
    savedDigest_ = saveDigest();   // a new, untouched document is not "modified"
}

Controller::~Controller() {
    // Order matters. bgfx presents into the surface, so the backend has to be down before the
    // surface goes; releasing first leaves the render thread holding a dangling layer for as long
    // as it takes to notice. Relying on member destruction order would work today and break the
    // day someone reorders the declarations.
    gpu_.reset();
    releaseSurface();
}

void Controller::releaseSurface() noexcept {
#if defined(__APPLE__)
    // Balances the alloc/init in createMetalLayerForView. NOT a teardown of the layer: the NSView
    // retains it too via setLayer:, so this drops our reference and leaves the view's intact.
    render::destroyMetalLayer(surface_);
#endif
    surface_ = nullptr;
}

void Controller::onDocumentChanged(std::function<void()> fn) { documentChanged_ = std::move(fn); }
void Controller::onViewChanged(std::function<void()> fn) { viewChanged_ = std::move(fn); }
void Controller::onStatus(std::function<void(const std::string&)> fn) { statusFn_ = std::move(fn); }

void Controller::notifyDocument() { if (documentChanged_) documentChanged_(); }
void Controller::notifyView() { if (viewChanged_) viewChanged_(); }
void Controller::status(const std::string& text) { if (statusFn_) statusFn_(text); }

// ── document ────────────────────────────────────────────────────────────────────────────

std::vector<TreeItem> Controller::tree() const {
    std::vector<TreeItem> out;
    const auto& doc = history_.current();
    for (const ObjectId id : doc.ids()) {
        const auto object = doc.find(id);
        if (!object) continue;
        TreeItem item;
        item.id = id;
        item.label = object->label();
        item.type = object->type();
        item.state = object->state();
        item.error = object->error().message;
        item.selected = std::find(selection_.begin(), selection_.end(), id) != selection_.end();
        item.visible = std::none_of(placements_.begin(), placements_.end(),
                                    [&](const render::Placement& p) {
                                        return p.object == id && !p.visible;
                                    });
        // Decided by TYPE here rather than by the shell, so a second shell cannot put the datums
        // somewhere else. When work planes and axes arrive they join this list and nothing else
        // has to change.
        if (object->type() == "Plane") item.group = TreeGroup::Origin;
        out.push_back(std::move(item));
    }
    return out;
}

CommandContext Controller::context() const {
    CommandContext ctx;
    ctx.selectedObjects = selection_.size();
    ctx.selectedElements = elementSelection_.size();
    ctx.documentEmpty = history_.current().size() == 0;
    ctx.canUndo = history_.canUndo();
    ctx.canRedo = history_.canRedo();
    return ctx;
}

void Controller::select(ObjectId id, bool additive) {
    // Element selection is part of THE selection, so a click in the tree replaces it rather than
    // leaving a face selected underneath a newly selected body.
    if (!additive) {
        selection_.clear();
        elementSelection_.clear();
    }
    const auto it = std::find(selection_.begin(), selection_.end(), id);
    if (it != selection_.end()) {
        selection_.erase(it);           // clicking a selected item again deselects it
    } else if (!id.isNull()) {
        selection_.push_back(id);
    }
    // So selecting in the TREE marks the geometry in the viewport. The two views showing different
    // selections is the kind of disagreement that makes a user distrust both.
    refreshHighlights();
    notifyDocument();
}

void Controller::setSelection(std::vector<ObjectId> ids) {
    // Nulls dropped rather than stored: a null id selects nothing, and keeping one would make
    // selection().size() disagree with what is actually selected.
    ids.erase(std::remove_if(ids.begin(), ids.end(),
                             [](ObjectId id) { return id.isNull(); }),
              ids.end());
    selection_ = std::move(ids);
    elementSelection_.clear();
    refreshHighlights();
    notifyDocument();
}

void Controller::clearSelection() {
    selection_.clear();
    elementSelection_.clear();
    refreshHighlights();
    notifyDocument();
}

void Controller::setPreferences(const Preferences& next) {
    preferences_ = next;
    // Pushed into the camera here rather than read by it: the camera is below app/ and must not
    // depend on a preferences type it cannot see.
    camera_.setPreset(preferences_.navigation);
    notifyDocument();   // property rows are formatted in the new units
    notifyView();
}

void Controller::rename(ObjectId id, const std::string& label) {
    const auto object = history_.current().find(id);
    if (!object || label.empty() || object->label() == label) return;
    auto next = history_.current().replace(
        std::make_shared<const document::ObjectData>(object->withLabel(label)));
    // A commit, so renaming is undoable. NOT invalidated: a label does not affect geometry, and
    // Document::digest deliberately excludes labels so a rename cannot throw away cached results.
    history_.commit(std::move(next), "Rename");
    notifyDocument();
    status("Renamed to " + label);
}

void Controller::setVisible(ObjectId id, bool visible) {
    for (auto& p : placements_) {
        if (p.object == id) p.visible = visible;
    }
    refresh();
}

void Controller::remove(ObjectId id) {
    auto next = history_.current();
    for (const ObjectId dep : next.dependents(id)) next = recompute::Engine::invalidate(next, dep);
    next = next.remove(id);
    history_.commit(std::move(next), "Delete");

    placements_.erase(std::remove_if(placements_.begin(), placements_.end(),
                                     [&](const render::Placement& p) { return p.object == id; }),
                      placements_.end());
    selection_.erase(std::remove(selection_.begin(), selection_.end(), id), selection_.end());
    refresh();
    status("Deleted");
}

bool Controller::undo() {
    if (!history_.undo()) {
        status("Nothing to undo");
        return false;
    }
    // Placements can reference objects the undone version does not have.
    const auto& doc = history_.current();
    placements_.erase(std::remove_if(placements_.begin(), placements_.end(),
                                     [&](const render::Placement& p) {
                                         return !doc.contains(p.object);
                                     }),
                      placements_.end());
    refresh();
    status("Undo");
    return true;
}

bool Controller::redo() {
    if (!history_.redo()) {
        status("Nothing to redo");
        return false;
    }
    refresh();
    status("Redo");
    return true;
}

void Controller::refresh() {
    recompute::Engine engine(registry_, cache_);
    auto result = engine.recompute(history_.current());
    if (!result) {
        // A cycle, typically. The message names the features involved.
        status(result.error().message);
        notifyDocument();
        return;
    }
    history_.replaceCurrent(std::move(result.value().first));
    failedCount_ = result.value().second.failed + result.value().second.blocked;

    // Drop element selections whose object no longer exists. An undo or a delete leaves the slot
    // pointing at whatever now occupies it, so keeping them would highlight unrelated geometry and
    // hand a command a reference to a feature that is gone.
    const auto orphaned = [this](const ElementSelection& e) {
        const auto found = history_.current().find(e.object);
        return !found || found->output() == nullptr;
    };
    elementSelection_.erase(
        std::remove_if(elementSelection_.begin(), elementSelection_.end(), orphaned),
        elementSelection_.end());

    // Only the TIP bodies are placed — an object that a later feature consumed is not drawn.
    //
    // Placing everything meant a fillet's result and the box it was built from were BOTH rendered,
    // occupying the same space, and the two z-fought into a dithered checkerboard that looked like
    // a shader bug. It was reported as one. Selecting the box then tinted its copy and the fight
    // became blue-and-grey, which is how it was found.
    //
    // It is also what a history-based modeller means: a feature CONSUMES its input. SolidWorks and
    // Inventor show the final body, not every intermediate one. Two independent boxes are both tips
    // and both stay visible, which is the case the old rule was right about.
    const auto& doc = history_.current();
    for (const ObjectId id : doc.ids()) {
        const auto object = doc.find(id);
        if (!object || object->output() == nullptr) continue;

        // Consumed if a dependent produced a SOLID that stands in this one's place.
        //
        // Two conditions, and both were learned from a bug:
        //
        // A dependent that FAILED consumes nothing, so its input stays visible — otherwise a broken
        // fillet makes the part disappear, which is the worst possible response to a failed
        // operation.
        //
        // A dependent that produced no SOLID consumes nothing either, and this is the one that was
        // missing. Consumption means REPLACEMENT: a fillet replaces the box it rounded. A sketch
        // placed on a face refers to its body — deliberately, so the face is a real dependency and
        // not a string — and replaces nothing at all. Under the old rule, starting a sketch on a
        // face made the body vanish, and finishing the sketch did not bring it back, because the
        // reference is permanent. Reported from the iPad as "the sketch is corrupting the data";
        // the data was intact and only the drawing was wrong.
        //
        // Measured by volume rather than by feature type, so this stays right for everything that
        // comes later: datum planes, construction geometry, and every surface feature that will
        // ever exist are all reference rather than replacement, and none of them has a volume.
        bool consumed = false;
        for (const ObjectId dependent : doc.dependents(id)) {
            const auto other = doc.find(dependent);
            if (other && other->output() != nullptr && other->output()->shape.volume() > 0.0) {
                consumed = true;
                break;
            }
        }

        const auto existing = std::find_if(placements_.begin(), placements_.end(),
                                           [&](const render::Placement& p) { return p.object == id; });
        if (consumed) {
            if (existing != placements_.end()) placements_.erase(existing);
            continue;
        }
        if (existing == placements_.end()) {
            render::Placement p;
            p.object = id;
            std::copy(kIdentity, kIdentity + 12, p.transform);
            // Datum planes start HIDDEN. They are reference geometry, not bodies: shown by default
            // they sit in front of the model as three large sheets, they are picked before the
            // faces behind them, and "fit" frames the datums rather than the part. Every CAD
            // application hides them for the same reasons and offers a toggle — which is what the
            // View tab's Origin Planes entry is for.
            //
            // Hidden, not absent: the object is in the tree, it can be selected there, and a
            // sketch can be placed on it. Visibility is about pixels, not about existence.
            p.visible = object->type() != "Plane";
            placements_.push_back(p);
        }
    }

    if (auto r = scene_->update(doc, placements_); !r) {
        status(r.error().message);
    }
    // After the rebuild, not before: update() resizes the highlight table to the new element count,
    // which drops what was marked. Without this a recompute silently unhighlights the selection --
    // and since every edit ends in a recompute, that is most of the time.
    refreshHighlights();
    scene_->setCamera(camera_.matrices(viewport_));

    if (failedCount_ > 0) {
        status(std::to_string(failedCount_) + " feature"
               + (failedCount_ == 1 ? "" : "s") + " could not be built");
    }
    notifyDocument();
    notifyView();
}

// ── properties ──────────────────────────────────────────────────────────────────────────

std::vector<Controller::PropertyRow> Controller::properties(ObjectId id) const {
    std::vector<PropertyRow> out;
    const auto object = history_.current().find(id);
    if (!object) return out;

    for (const auto& p : object->properties()) {
        PropertyRow row;
        row.name = p.name;
        row.type = document::toString(document::typeOf(p.value));
        // Formatted for a human, units included. The shell must not have to know that a Length
        // is stored in millimetres.
        if (const auto* length = std::get_if<units::Length>(&p.value)) {
            row.value = units::format(*length, preferences_.displayUnits);
        } else if (const auto* angle = std::get_if<units::Angle>(&p.value)) {
            row.value = units::format(*angle);
        } else {
            row.value = document::toString(p.value);
        }
        // Element references are shown but not hand-editable: a user picks geometry, they do not
        // type "B0.0#0[a3f2,91c4]".
        row.editable = document::typeOf(p.value) != document::PropertyType::Element
                       && document::typeOf(p.value) != document::PropertyType::ElementList
                       && document::typeOf(p.value) != document::PropertyType::Object;
        out.push_back(std::move(row));
    }
    return out;
}

bool Controller::setProperty(ObjectId id, const std::string& name, const std::string& text) {
    const auto object = history_.current().find(id);
    if (!object) return false;
    const auto* existing = object->find(name);
    if (existing == nullptr) return false;

    document::PropertyValue value;
    switch (document::typeOf(*existing)) {
        case document::PropertyType::Length: {
            // A bare number is read in the DISPLAY units, so what you type back matches what you
        // just read. Typing "2" into a field showing inches must mean two inches.
        auto parsed = units::parseLength(text, preferences_.displayUnits);
            if (!parsed) {
                status(parsed.error().message);   // "'10 furlongs' is not a unit I recognise."
                return false;
            }
            value = parsed.value();
            break;
        }
        case document::PropertyType::Angle: {
            auto parsed = units::parseAngle(text);
            if (!parsed) {
                status(parsed.error().message);
                return false;
            }
            value = parsed.value();
            break;
        }
        case document::PropertyType::Real:
            try {
                value = std::stod(text);
            } catch (...) {
                status("'" + text + "' is not a number.");
                return false;
            }
            break;
        case document::PropertyType::Int:
            try {
                value = static_cast<std::int64_t>(std::stoll(text));
            } catch (...) {
                status("'" + text + "' is not a whole number.");
                return false;
            }
            break;
        case document::PropertyType::Bool:
            value = (text == "true" || text == "1" || text == "yes");
            break;
        case document::PropertyType::Text:
            value = text;
            break;
        default:
            return false;   // references are picked, not typed
    }

    auto updated = object->withProperty(name, std::move(value));
    auto next = history_.current().replace(
        std::make_shared<const document::ObjectData>(std::move(updated)));
    next = recompute::Engine::invalidate(next, id);
    history_.commit(std::move(next), "Change " + name);
    refresh();
    return true;
}

// ── viewport ────────────────────────────────────────────────────────────────────────────

void Controller::setRollback(std::optional<ObjectId> marker) {
    // replaceCurrent, not commit: see the header. Moving the marker is navigation, not an edit.
    history_.replaceCurrent(history_.current().withRollbackAfter(marker));
    // Placements for suspended features must go, or their last-known geometry keeps being drawn.
    // refresh() rebuilds the scene from objects that still have output, so dropping placements whose
    // object lost its output is what actually makes the viewport reflect the rollback.
    refresh();
    const auto& doc = history_.current();
    placements_.erase(std::remove_if(placements_.begin(), placements_.end(),
                                     [&](const render::Placement& p) {
                                         const auto o = doc.find(p.object);
                                         return !o || o->output() == nullptr;
                                     }),
                      placements_.end());
    refresh();
    if (marker.has_value()) {
        const auto object = doc.find(*marker);
        status("Rolled back to " + (object ? object->label() : std::string("a feature")));
    } else {
        status("Rolled forward to the end");
    }
}

bool Controller::isRolledBack(ObjectId id) const {
    return history_.current().isRolledBack(id);
}

std::optional<ObjectId> Controller::rollback() const {
    return history_.current().rollbackAfter();
}

}  // namespace cad::app
