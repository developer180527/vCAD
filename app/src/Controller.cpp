#include "cad/app/Controller.h"

#include "cad/io/DocumentStore.h"
#include "cad/sketch/Sketch.h"

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
    savedDigest_ = saveDigest();   // a new, untouched document is not "modified"
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
        // instanceCount, not the visible ranges: this is the status bar's "how big is this
        // model" figure, and a number that dropped every time the user orbited would read as
        // geometry going missing.
        s.triangles += (batch.indexCount / 3) * batch.instanceCount;
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

std::vector<naming::ElementName> Controller::edgesOf(ObjectId id) const {
    std::vector<naming::ElementName> edges;
    const auto object = history_.current().find(id);
    if (!object || object->output() == nullptr) return edges;
    const auto& output = *object->output();

    // Filter by resolved shape type rather than by anything in the name. A name records
    // provenance, not topology — an edge and the face that bounds it can both come from the same
    // feature and the same operation, so only the shape it resolves to tells them apart.
    for (const auto& name : output.map.allNames()) {
        const auto shape = output.map.resolve(name);
        if (shape && shape->type() == kernel::ShapeType::Edge) edges.push_back(name);
    }
    return edges;
}

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

std::optional<ObjectId> Controller::rollback() const {
    return history_.current().rollbackAfter();
}

ObjectId Controller::addSketch() {
    // A closed, fully constrained 40 x 25 rectangle on XY. Constrained rather than merely drawn:
    // the point of a sketch is that its dimensions drive it, and a seed with 8 free degrees of
    // freedom would extrude fine and then behave nothing like a sketch when edited.
    sketch::Sketch sk(sketch::Plane::XY);
    const auto bottom = sk.addLine(0, 0, 40, 0);
    const auto right = sk.addLine(40, 0, 40, 25);
    const auto top = sk.addLine(40, 25, 0, 25);
    const auto left = sk.addLine(0, 25, 0, 0);
    using PR = sketch::PointRef;
    sk.coincident(bottom, PR::End, right, PR::Start);
    sk.coincident(right, PR::End, top, PR::Start);
    sk.coincident(top, PR::End, left, PR::Start);
    sk.coincident(left, PR::End, bottom, PR::Start);
    sk.horizontal(bottom);
    sk.horizontal(top);
    sk.vertical(left);
    sk.vertical(right);
    sk.lockX(bottom, PR::Start, 0.0);
    sk.lockY(bottom, PR::Start, 0.0);
    sk.distance(bottom, PR::Start, bottom, PR::End, 40.0);
    sk.distance(right, PR::Start, right, PR::End, 25.0);
    sk.solve();

    auto [next, id] = history_.current().add("Sketch");
    const auto object = next.find(id);
    auto updated = object->withProperty("sketch", sk.serialize())
                       .withProperty("plane", static_cast<std::int64_t>(sketch::Plane::XY));
    next = next.replace(std::make_shared<const document::ObjectData>(std::move(updated)));
    history_.commit(std::move(next), "Sketch");

    selection_.clear();
    selection_.push_back(id);
    refresh();
    if (history_.current().size() == 1) fitView();
    status("Added Sketch — a placeholder 40 x 25 rectangle until the sketch editor exists");
    return id;
}

void Controller::addExtrude(double millimetres) {
    if (selection_.size() != 1) return;
    const ObjectId profile = selection_.front();
    const auto object = history_.current().find(profile);
    if (!object || object->type() != "Sketch") {
        status("Extrude needs a sketch selected.");
        return;
    }

    // The direction comes from the sketch's plane, so carry it across. Extrude reads it from its own
    // property rather than re-parsing the sketch text on every recompute.
    std::int64_t plane = 0;
    if (const auto* stored = object->find("plane")) {
        if (const auto* v = std::get_if<std::int64_t>(stored)) plane = *v;
    }

    auto [next, id] = history_.current().add("Extrude");
    const auto created = next.find(id);
    auto updated = created->withProperty("a_profile", profile)
                       .withProperty("distance", units::millimetres(millimetres))
                       .withProperty("plane", plane);
    next = next.replace(std::make_shared<const document::ObjectData>(std::move(updated)));
    history_.commit(std::move(next), "Extrude");

    selection_.clear();
    selection_.push_back(id);
    refresh();

    const auto result = history_.current().find(id);
    if (result && result->output() == nullptr) {
        status("Extrude failed — see the feature's error in the browser.");
    } else {
        status("Extruded " + std::to_string(static_cast<int>(millimetres)) + " mm");
    }
}

void Controller::addBoolean(const std::string& type, const std::string& label) {
    if (selection_.size() != 2) return;
    auto [next, id] = history_.current().add(type);
    const auto object = next.find(id);
    // Property names order the inputs: "a_base" sorts before "b_tool", and the engine passes
    // inputs in property order.
    auto updated = object->withProperty("a_base", selection_[0])
                       .withProperty("b_tool", selection_[1]);
    next = next.replace(std::make_shared<const document::ObjectData>(std::move(updated)));
    history_.commit(std::move(next), label);
    selection_.clear();
    selection_.push_back(id);
    refresh();
    status(label);
}

void Controller::addEdgeFeature(const std::string& type, const std::string& label,
                                const std::string& sizeProperty, double millimetres) {
    if (selection_.size() != 1) return;
    const ObjectId target = selection_.front();

    auto edges = edgesOf(target);
    if (edges.empty()) {
        status("Nothing to " + label + ": that feature has no edges yet.");
        return;
    }

    auto [next, id] = history_.current().add(type);
    const auto object = next.find(id);
    auto updated = object->withProperty("a_base", target)
                       .withProperty(sizeProperty, units::millimetres(millimetres))
                       .withProperty("edges", std::move(edges));
    next = next.replace(std::make_shared<const document::ObjectData>(std::move(updated)));
    history_.commit(std::move(next), label);
    selection_.clear();
    selection_.push_back(id);
    refresh();

    // Report what actually happened. A fillet that silently failed on 3 of 12 edges is the kind
    // of thing the engine records and the user would otherwise never learn: partial failure is a
    // feature of the recompute engine, so it has to be a feature of the status line too.
    const auto result = history_.current().find(id);
    if (result && result->output() == nullptr) {
        status(label + " failed — see the feature's error in the browser.");
    } else {
        status(label + " applied to all edges (edge selection is not wired yet).");
    }
}

std::uint64_t Controller::saveDigest() const noexcept {
    // Document::digest() covers ids, types and property values — deliberately NOT labels, because
    // it feeds the recompute cache key and renaming a feature must not invalidate cached geometry.
    // But a rename IS a change worth saving, so folding labels in here is the difference between
    // "close without saving" losing a rename and preserving it. Two digests with two jobs.
    std::uint64_t h = history_.current().digest();
    const auto& doc = history_.current();
    for (const auto id : doc.ids()) {
        const auto object = doc.find(id);
        if (!object) continue;
        for (const char c : object->label()) {
            h ^= static_cast<std::uint64_t>(c);
            h *= 1099511628211ULL;
        }
    }
    return h;
}

bool Controller::modified() const noexcept { return saveDigest() != savedDigest_; }

kernel::Result<void> Controller::saveTo(const std::filesystem::path& path,
                                       const std::string& kind) {
    auto r = io::saveDocument(history_.current(), path, kind);
    if (!r) return r.error();
    savedDigest_ = saveDigest();
    notifyDocument();   // the title bar's dirty marker is derived from modified()
    status("Saved " + path.filename().string());
    return {};
}

kernel::Result<void> Controller::loadFrom(const std::filesystem::path& path) {
    auto loaded = io::loadDocument(path);
    if (!loaded) return loaded.error();

    // A fresh History rather than a commit. Recompute happens through refresh() below, which is
    // the same path every edit takes, so a document that opens with a failed feature reports it
    // exactly like one that acquired the failure interactively.
    history_ = document::History{std::move(loaded.value())};
    selection_.clear();
    refresh();
    savedDigest_ = saveDigest();
    fitView();
    status("Opened " + path.filename().string());
    return {};
}

kernel::Result<ObjectId> Controller::importFile(const std::filesystem::path& path) {
    auto [next, id] = history_.current().add("Import");
    const auto object = next.find(id);
    auto updated = object->withProperty("path", path.string());
    next = next.replace(std::make_shared<const document::ObjectData>(std::move(updated)));

    // Recompute BEFORE committing. An import that cannot be read must not land in history: the
    // user would get an undo step for a feature that never produced geometry, and every later
    // recompute would retry the same unreadable file.
    recompute::Engine engine(registry_, cache_);
    auto computed = engine.recompute(next);
    if (!computed) return computed.error();

    next = computed.value().first;
    const auto imported = next.find(id);
    if (!imported || imported->output() == nullptr) {
        return kernel::Error{kernel::ErrorCode::InvalidInput,
                             "That file could not be imported.",
                             path.string()};
    }

    history_.commit(std::move(next), "Import " + path.filename().string());
    selection_.clear();
    selection_.push_back(id);
    refresh();
    fitView();
    status("Imported " + path.filename().string());
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

    // Booleans. Inventor exposes all three as modes of one "Combine" command; we register them
    // separately so each is reachable now, and the mode selector can fold them together once the
    // non-modal command surface exists (DESKTOP_UX 3.2).
    const auto twoSelected = [](const CommandContext& c) { return c.selectedObjects == 2; };
    commands_.push_back({"edit.rollback", "Roll Back",
                         "Suspend every feature after the selected one", "rollback",
                         [](const CommandContext& c) { return c.selectedObjects == 1; },
                         [this] {
                             if (selection_.size() == 1) setRollback(selection_.front());
                         }});
    commands_.push_back({"edit.rollforward", "Roll Forward",
                         "Compute the whole feature tree again", "rollforward",
                         [this](const CommandContext&) {
                             return history_.current().rollbackAfter().has_value();
                         },
                         [this] { setRollback(std::nullopt); }});

    commands_.push_back({"feature.sketch", "Start Sketch",
                         "Create a sketch on the XY plane", "sketch", always,
                         [this] { addSketch(); }});
    commands_.push_back({"feature.extrude", "Extrude",
                         "Extrude the selected sketch into a solid", "extrude",
                         [this](const CommandContext& c) {
                             // Enabled only for a SKETCH selection. A generic "one thing selected"
                             // predicate would offer Extrude on a box, which then fails -- and a
                             // button that lights up and then refuses is worse than a dim one.
                             if (c.selectedObjects != 1) return false;
                             const auto object = history_.current().find(selection_.front());
                             return object != nullptr && object->type() == "Sketch";
                         },
                         [this] { addExtrude(10.0); }});

    commands_.push_back({"feature.cut", "Cut", "Subtract the second selection from the first",
                         "cut", twoSelected, [this] { addBoolean("Cut", "Cut"); }});
    commands_.push_back({"feature.fuse", "Join", "Merge the selected bodies into one", "combine",
                         twoSelected, [this] { addBoolean("Fuse", "Join"); }});
    commands_.push_back({"feature.common", "Intersect",
                         "Keep only the volume the selected bodies share", "combine", twoSelected,
                         [this] { addBoolean("Common", "Intersect"); }});

    // Edge features. Enabled on a single selection that HAS edges — asking for a fillet on a
    // feature with no computed output should not offer itself as available.
    const auto oneWithEdges = [this](const CommandContext& c) {
        return c.selectedObjects == 1 && !edgesOf(selection_.front()).empty();
    };
    commands_.push_back({"feature.fillet", "Fillet", "Round every edge of the selected body",
                         "fillet", oneWithEdges,
                         [this] { addEdgeFeature("Fillet", "Fillet", "radius", 5.0); }});
    commands_.push_back({"feature.chamfer", "Chamfer", "Bevel every edge of the selected body",
                         "chamfer", oneWithEdges,
                         [this] { addEdgeFeature("Chamfer", "Chamfer", "distance", 3.0); }});

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
