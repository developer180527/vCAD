#include "cad/app/Controller.h"

#include "cad/units/Units.h"

#include <algorithm>

namespace cad::app {
namespace {

using document::ObjectId;
using document::ObjectState;

/// Naming serial for a placement's transform. Identity for now: assemblies come later, and a
/// placement that lies about its transform is worse than one that has none.
constexpr float kIdentity[12]{1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};

}  // namespace

Controller::Controller() {
    meshes_ = std::make_unique<render::MeshCache>(blobs_);
    scene_ = std::make_unique<render::SceneBuilder>(*meshes_, backend_.resources);
    viewport_.width = 1280;
    viewport_.height = 800;
    scene_->setViewport(viewport_);
    registerCommands();
}

Controller::~Controller() = default;

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
        out.push_back(std::move(item));
    }
    return out;
}

CommandContext Controller::context() const {
    CommandContext ctx;
    ctx.selectedObjects = selection_.size();
    ctx.documentEmpty = history_.current().size() == 0;
    ctx.canUndo = history_.canUndo();
    ctx.canRedo = history_.canRedo();
    return ctx;
}

void Controller::select(ObjectId id, bool additive) {
    if (!additive) selection_.clear();
    const auto it = std::find(selection_.begin(), selection_.end(), id);
    if (it != selection_.end()) {
        selection_.erase(it);           // clicking a selected item again deselects it
    } else if (!id.isNull()) {
        selection_.push_back(id);
    }
    notifyDocument();
}

void Controller::clearSelection() {
    selection_.clear();
    notifyDocument();
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

    // Every computed object gets a placement if it does not have one. Real assemblies will place
    // deliberately; for a part document, "everything is visible once" is what a user expects.
    const auto& doc = history_.current();
    for (const ObjectId id : doc.ids()) {
        const auto object = doc.find(id);
        if (!object || object->output() == nullptr) continue;
        const bool known = std::any_of(placements_.begin(), placements_.end(),
                                       [&](const render::Placement& p) { return p.object == id; });
        if (!known) {
            render::Placement p;
            p.object = id;
            std::copy(kIdentity, kIdentity + 12, p.transform);
            placements_.push_back(p);
        }
    }

    if (auto r = scene_->update(doc, placements_); !r) {
        status(r.error().message);
    }
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
            row.value = units::format(*length, units::UnitSystem::Millimetre);
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
            auto parsed = units::parseLength(text, units::UnitSystem::Millimetre);
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

void Controller::setViewportSize(std::uint32_t width, std::uint32_t height) {
    if (width == 0 || height == 0) return;
    viewport_.width = width;
    viewport_.height = height;
    scene_->setViewport(viewport_);
    scene_->setCamera(camera_.matrices(viewport_));
    notifyView();
}

void Controller::fitView() {
    camera_.fit(scene_->bounds(), viewport_);
    scene_->setCamera(camera_.matrices(viewport_));
    notifyView();
}

Controller::Stats Controller::stats() const {
    Stats s;
    s.objects = history_.current().size();
    s.uniqueMeshes = scene_->stats().uniqueMeshes;
    s.instances = scene_->stats().instances;
    s.failed = failedCount_;
    for (const auto& batch : scene_->frame().batches) {
        s.triangles += (batch.indexCount / 3) * batch.instances.size();
    }
    return s;
}

// ── commands ────────────────────────────────────────────────────────────────────────────

ObjectId Controller::addPrimitive(const std::string& type,
                                  const std::vector<std::pair<std::string, double>>& lengths) {
    auto [next, id] = history_.current().add(type);
    auto object = next.find(id);
    auto updated = *object;
    for (const auto& [name, mm] : lengths) {
        updated = updated.withProperty(name, units::millimetres(mm));
    }
    next = next.replace(std::make_shared<const document::ObjectData>(std::move(updated)));
    history_.commit(std::move(next), "Add " + type);

    selection_.clear();
    selection_.push_back(id);
    refresh();
    // Framing on the first object is the difference between "it worked" and "nothing happened":
    // a new box outside the current view looks like a no-op.
    if (history_.current().size() == 1) fitView();
    status("Added " + type);
    return id;
}

void Controller::registerCommands() {
    const auto always = [](const CommandContext&) { return true; };

    commands_.push_back({"feature.box", "Box", "Create a rectangular block", "box", always,
                         [this] { addPrimitive("Box", {{"dx", 100}, {"dy", 60}, {"dz", 40}}); }});
    commands_.push_back({"feature.cylinder", "Cylinder", "Create a cylinder", "cylinder", always,
                         [this] {
                             addPrimitive("Cylinder", {{"radius", 25}, {"height", 80}});
                         }});

    commands_.push_back({"feature.cut", "Cut", "Subtract the second selection from the first",
                         "cut",
                         [](const CommandContext& c) { return c.selectedObjects == 2; },
                         [this] {
                             if (selection_.size() != 2) return;
                             auto [next, id] = history_.current().add("Cut");
                             auto object = next.find(id);
                             // Property names order the inputs: "a_base" sorts before "b_tool",
                             // and the engine passes inputs in property order.
                             auto updated = object->withProperty("a_base", selection_[0])
                                                .withProperty("b_tool", selection_[1]);
                             next = next.replace(std::make_shared<const document::ObjectData>(
                                 std::move(updated)));
                             history_.commit(std::move(next), "Cut");
                             selection_.clear();
                             selection_.push_back(id);
                             refresh();
                             status("Cut");
                         }});

    commands_.push_back({"edit.delete", "Delete", "Delete the selected features", "delete",
                         [](const CommandContext& c) { return c.selectedObjects > 0; },
                         [this] {
                             const auto ids = selection_;
                             for (const ObjectId id : ids) remove(id);
                         }});

    commands_.push_back({"edit.undo", "Undo", "Undo the last change", "undo",
                         [](const CommandContext& c) { return c.canUndo; },
                         [this] { undo(); }});
    commands_.push_back({"edit.redo", "Redo", "Redo the last undone change", "redo",
                         [](const CommandContext& c) { return c.canRedo; },
                         [this] { redo(); }});

    commands_.push_back({"view.fit", "Fit", "Frame everything in the view", "fit",
                         [](const CommandContext& c) { return !c.documentEmpty; },
                         [this] {
                             fitView();
                             status("Fit to view");
                         }});
    commands_.push_back({"view.ortho", "Orthographic", "Toggle orthographic projection", "ortho",
                         always,
                         [this] {
                             const bool next = !camera_.orthographic();
                             camera_.setOrthographic(next);
                             scene_->setCamera(camera_.matrices(viewport_));
                             notifyView();
                             status(next ? "Orthographic" : "Perspective");
                         }});
}

}  // namespace cad::app
