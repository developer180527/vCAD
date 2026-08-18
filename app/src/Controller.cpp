#include "cad/app/Controller.h"

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

using document::ObjectId;
using document::ObjectState;

/// Naming serial for a placement's transform. Identity for now: assemblies come later, and a
/// placement that lies about its transform is worse than one that has none.
constexpr float kIdentity[12]{1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};

}  // namespace

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

        // Consumed if anything that depends on it actually produced a body. A dependent that FAILED
        // consumes nothing, so its input stays visible — otherwise a broken fillet would make the
        // part disappear, which is the worst possible response to a failed operation.
        bool consumed = false;
        for (const ObjectId dependent : doc.dependents(id)) {
            const auto other = doc.find(dependent);
            if (other && other->output() != nullptr) {
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

void Controller::setViewportSize(std::uint32_t width, std::uint32_t height) {
    if (width == 0 || height == 0) return;
    viewport_.width = width;
    viewport_.height = height;
    scene_->setViewport(viewport_);
    scene_->setCamera(camera_.matrices(viewport_));
    // The backend owns the framebuffer being read back, so it has to be told too. Skipping this
    // reads back the OLD size and the shell blits a stale, wrongly-shaped image.
#if defined(__APPLE__)
    // Before the backend's resize: bgfx::reset builds a swap chain against the layer, and a layer
    // still at the old drawableSize gives a surface that disagrees with the backbuffer.
    if (surface_ != nullptr) render::resizeMetalLayer(surface_, width, height, viewportScale_);
#endif
    if (active_.frames != nullptr) active_.frames->resize(viewport_);
    notifyView();
}

void Controller::cameraChanged() {
    scene_->setCamera(camera_.matrices(viewport_));
    notifyView();
}

void Controller::fitView() {
    camera_.fit(scene_->bounds(), viewport_);
    scene_->setCamera(camera_.matrices(viewport_));
    notifyView();
}

void Controller::setViewportBackground(int r, int g, int b) {
    scene_->setBackground(float(r) / 255.0f, float(g) / 255.0f, float(b) / 255.0f);
    notifyView();
}

// ── the GPU renderer ────────────────────────────────────────────────────────────────────

kernel::Result<void> Controller::attachRenderer(std::uint32_t width, std::uint32_t height,
                                                void* nativeView, double scale) {
    // bgfx is a process singleton, so this is idempotent rather than an error: two viewports
    // share one backend and differ by view id.
    if (gpu_) return {};

    // A failed attempt must not leak into the next one. The shell's fallback retries with
    // nativeView = nullptr expecting the offscreen path, and a surface left over from the native
    // attempt would silently rebuild the SAME on-screen configuration — so the fallback would
    // fail identically, or succeed on-screen while the shell believed it was blitting.
    releaseSurface();

    auto gpu = std::make_unique<render::BgfxBackend>();
    const std::uint32_t w = width != 0 ? width : viewport_.width;
    const std::uint32_t h = height != 0 ? height : viewport_.height;

    // Outside the platform guard: every platform needs the scale for later layer/backbuffer
    // resizes, and keeping it Apple-only means Windows and X11 silently render at 1.0 on a
    // high-DPI display.
    viewportScale_ = scale;

#if defined(__APPLE__)
    // A CAMetalLayer, never the view: bgfx::init parks the calling thread waiting for the render
    // thread, and given a view the render thread must build the layer on the main thread and
    // wait for it. Both wait forever. See render/src/MetalSurface.mm.
    if (nativeView != nullptr) surface_ = render::createMetalLayerForView(nativeView, w, h, scale);
#else
    // Windows and X11 take the window handle directly; no intermediate surface object.
    surface_ = nativeView;
#endif

    render::BgfxConfig config;
    config.nativeWindow = surface_;
    // Mutually exclusive, and bgfx enforces it: offscreen init demands a 0x0 backbuffer, so a
    // surface handle and offscreen mode cannot both be set.
    config.offscreen = surface_ == nullptr;
    config.viewport.width = w;
    config.viewport.height = h;

    if (auto r = gpu->initialise(config); !r) {
        releaseSurface();
        return r.error();
    }

    // Noop is not a failure to bgfx: it validates every call and draws nothing, so init succeeds
    // and the frame comes back blank with no error anywhere. Refuse it here instead, because a
    // viewport that silently renders nothing is the single most expensive bug this project has
    // had, twice.
    if (gpu->rendererName() == "Noop") {
        gpu->shutdown();
        releaseSurface();
        return kernel::Error{kernel::ErrorCode::Internal,
                             "No GPU renderer available; the viewport would draw nothing.",
                             "bgfx selected the Noop renderer"};
    }

    gpu_ = std::move(gpu);
    active_ = gpu_->handle();
    presenting_ = surface_ != nullptr;

    // Before any camera matrix is built. The two conventions differ by renderer, and getting it
    // wrong depth-clips the whole scene and draws nothing — with no error.
    camera_.setHomogeneousDepth(gpu_->homogeneousDepth());

    // A SceneBuilder holds buffer ids issued by the resources it was built against, so it cannot
    // be pointed at a different backend. Rebuild it and re-upload; at startup the document is
    // usually empty, and when it is not this is a one-off cost at attach.
    scene_ = std::make_unique<render::SceneBuilder>(*meshes_, *active_.resources);
    viewport_ = config.viewport;
    scene_->setViewport(viewport_);
    active_.frames->resize(viewport_);
    if (auto r = scene_->update(history_.current(), placements_); !r) return r.error();
    scene_->setCamera(camera_.matrices(viewport_));

    notifyView();
    return {};
}

std::string Controller::rendererName() const {
    return gpu_ ? gpu_->rendererName() : std::string("null");
}

void Controller::presentFrame() {
    if (!gpu_ || !presenting_) return;
    const auto t0 = std::chrono::steady_clock::now();
    active_.frames->submit(scene_->frame());
    timing_.submitMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    timing_.captureMs = 0.0;   // the entire point of this path
}

kernel::Result<Controller::RenderedFrame> Controller::renderFrame() {
    if (!gpu_) {
        return kernel::Error{kernel::ErrorCode::InvalidInput,
                             "No renderer is attached.",
                             "call attachRenderer() first"};
    }
    const auto t0 = std::chrono::steady_clock::now();
    active_.frames->submit(scene_->frame());
    const auto t1 = std::chrono::steady_clock::now();
    auto pixels = gpu_->captureFrame();
    const auto t2 = std::chrono::steady_clock::now();
    using Ms = std::chrono::duration<double, std::milli>;
    timing_.submitMs = Ms(t1 - t0).count();
    timing_.captureMs = Ms(t2 - t1).count();
    if (!pixels) return pixels.error();

    RenderedFrame out;
    out.width = viewport_.width;
    out.height = viewport_.height;
    out.pixels = std::move(pixels.value());
    return out;
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

Controller::Pick Controller::pickAt(std::uint32_t x, std::uint32_t y) {
    Pick out;
    if (active_.picker == nullptr) return out;

    const auto raw = active_.picker->pick(scene_->frame(), x, y);
    if (!raw.valid) return out;

    out.hit = true;
    out.slot = raw.element;
    out.depth = raw.depth;
    if (const auto name = scene_->resolve(raw)) out.element = *name;
    if (const auto owner = scene_->objectOf(raw.element)) out.object = *owner;
    return out;
}

kernel::Result<Controller::FacePick> Controller::pickSketchFace(std::uint32_t x, std::uint32_t y) {
    const Pick pick = pickAt(x, y);
    if (!pick.hit) {
        return kernel::Error{kernel::ErrorCode::NotDone,
                             "Click a flat face to sketch on it."};
    }
    if (pick.element.isNull()) {
        // A slot with no name is a real state, not a bug: the mesh carries a name per element and
        // an element the naming layer never named comes back null. Saying "nothing there" would be
        // a lie about a place the user can see geometry.
        return kernel::Error{kernel::ErrorCode::NamingLost,
                             "That geometry cannot be referred to, so a sketch cannot be attached "
                             "to it."};
    }

    const auto object = history_.current().find(pick.object);
    if (!object || object->output() == nullptr) {
        return kernel::Error{kernel::ErrorCode::NotDone,
                             "That body has not been computed yet."};
    }

    const auto shape = object->output()->map.resolve(pick.element);
    if (!shape) {
        return kernel::Error{kernel::ErrorCode::NamingLost,
                             "That face no longer exists in the model.",
                             "could not resolve element '" + pick.element.toString() + "'"};
    }
    if (shape->type() != kernel::ShapeType::Face) {
        // Checked by resolved TOPOLOGY, not by anything in the name -- the same reason edgesOf
        // gives. An edge and the face bounding it can share a feature and an operation, so the
        // name cannot tell them apart and only the shape can.
        return kernel::Error{kernel::ErrorCode::InvalidInput,
                            "A sketch needs a face. That is an edge or a vertex."};
    }

    // The refusal that matters, and it is not ours: kernel::planeOf measures the surface and
    // refuses a cylinder or a sphere with its own message. Surfaced rather than swallowed, because
    // a click on the round side of a cylinder that quietly does nothing is indistinguishable from
    // a broken picker.
    const auto measured = kernel::planeOf(*shape);
    if (!measured) return measured.error();

    FacePick out;
    out.object = pick.object;
    out.face = pick.element;
    // Two structurally identical types kept apart so core/sketch need not know the kernel, exactly
    // as the Sketch feature does when the recompute resolves the same reference. This layer sees
    // both, so the copy is legitimate here.
    for (int i = 0; i < 3; ++i) {
        out.frame.origin[i] = measured.value().origin[i];
        out.frame.u[i] = measured.value().u[i];
        out.frame.v[i] = measured.value().v[i];
    }
    return out;
}

void Controller::alignViewTo(const sketch::SketchFrame& frame) {
    // Narrowed to float here rather than anywhere lower: the document and the kernel work in
    // doubles, the GPU pipeline in floats, and this is the boundary between them.
    const float origin[3]{static_cast<float>(frame.origin[0]), static_cast<float>(frame.origin[1]),
                          static_cast<float>(frame.origin[2])};
    const auto n = frame.normal();
    const float normal[3]{static_cast<float>(n[0]), static_cast<float>(n[1]),
                          static_cast<float>(n[2])};
    const float up[3]{static_cast<float>(frame.v[0]), static_cast<float>(frame.v[1]),
                      static_cast<float>(frame.v[2])};

    camera_.alignTo(origin, normal, up);
    cameraChanged();
}

const char* toString(Controller::SelectionLevel level) noexcept {
    switch (level) {
        case Controller::SelectionLevel::Body:   return "Body";
        case Controller::SelectionLevel::Face:   return "Face";
        case Controller::SelectionLevel::Edge:   return "Edge";
        case Controller::SelectionLevel::Vertex: return "Vertex";
    }
    return "Body";
}

namespace {

/// The topology a selection level resolves to. Body has none: it selects a document object, not a
/// piece of geometry, which is why it is handled separately everywhere below.
std::optional<kernel::ShapeType> topologyFor(Controller::SelectionLevel level) {
    switch (level) {
        case Controller::SelectionLevel::Face:   return kernel::ShapeType::Face;
        case Controller::SelectionLevel::Edge:   return kernel::ShapeType::Edge;
        case Controller::SelectionLevel::Vertex: return kernel::ShapeType::Vertex;
        case Controller::SelectionLevel::Body:   return std::nullopt;
    }
    return std::nullopt;
}

}  // namespace

bool Controller::selectElement(ObjectId id, const naming::ElementName& element, bool additive) {
    const auto object = history_.current().find(id);
    if (!object || object->output() == nullptr) return false;
    // Checked against the map rather than taken on trust: a name that does not resolve would sit
    // in the selection looking valid and fail later, far from here.
    if (!object->output()->map.resolve(element)) return false;

    if (!additive) {
        elementSelection_.clear();
        selection_.clear();
    }
    elementSelection_.push_back(ElementSelection{id, element, 0});
    if (std::find(selection_.begin(), selection_.end(), id) == selection_.end()) {
        selection_.push_back(id);
    }
    refreshHighlights();
    notifyDocument();
    return true;
}

void Controller::setSelectionLevel(SelectionLevel level) {
    if (level == selectionLevel_) return;
    selectionLevel_ = level;
    // Dropped rather than converted. There is no honest mapping from "this face" to "this edge", and
    // keeping a face selected while the level reads Edge is how a command acts on something the user
    // cannot see is selected.
    elementSelection_.clear();
    refreshHighlights();
    notifyDocument();
}

std::vector<std::uint32_t> Controller::slotsOf(ObjectId id) const {
    std::vector<std::uint32_t> slots;
    if (id.isNull()) return slots;
    // Walked rather than looked up: the scene maps slot -> object, not the reverse. Only ever run on
    // a selection change, which is a human-speed event, so a reverse index would be memory spent to
    // speed up something nobody is waiting for.
    for (std::uint32_t slot = 0; slot < scene_->frame().elementCount; ++slot) {
        const auto owner = scene_->objectOf(slot);
        if (owner && *owner == id) slots.push_back(slot);
    }
    return slots;
}

void Controller::refreshHighlights() {
    scene_->clearHighlights();

    // Selection first, hover second, so hovering something already selected reads as hovered. The
    // opposite order makes the pointer appear to do nothing over a selected face.
    if (selectionLevel_ == SelectionLevel::Body) {
        for (const ObjectId id : selection_) {
            for (const std::uint32_t slot : slotsOf(id)) {
                scene_->setHighlight(slot, render::Highlight::Selected);
            }
        }
    } else {
        for (const ElementSelection& picked : elementSelection_) {
            scene_->setHighlight(picked.slot, render::Highlight::Selected);
        }
    }

    if (hoveredSlot_) scene_->setHighlight(*hoveredSlot_, render::Highlight::Hovered);

    // A view change, not a document change: nothing about the model moved, but the pixels differ.
    // Without this, selecting in the model tree updated the highlight table and never asked anyone
    // to repaint, so the viewport kept showing the previous frame -- which looks exactly like
    // selection not working.
    notifyView();
}

Controller::ClickResult Controller::clickAt(std::uint32_t x, std::uint32_t y, bool additive) {
    ClickResult out;
    const Pick pick = pickAt(x, y);

    if (!pick.hit) {
        // Empty space. Clearing is what every CAD application does, and it is what keeps the tree
        // and the viewport agreeing about what is selected.
        if (additive) return out;
        const bool had = !selection_.empty() || !elementSelection_.empty();
        selection_.clear();
        elementSelection_.clear();
        out.changed = had;
        if (had) {
            refreshHighlights();
            notifyDocument();
        }
        return out;
    }

    out.hit = true;

    if (selectionLevel_ == SelectionLevel::Body) {
        if (pick.object.isNull()) {
            out.message = "That geometry does not belong to a feature.";
            return out;
        }
        select(pick.object, additive);       // notifies on its own
        refreshHighlights();
        out.changed = true;
        const auto object = history_.current().find(pick.object);
        out.message = object ? "Selected " + object->label() : "Selected";
        return out;
    }

    // Below Body: resolve the picked element's TOPOLOGY and refuse anything of the wrong kind. By
    // resolved shape, not by the name -- an edge and the face bounding it can come from the same
    // feature and the same operation, so the name cannot tell them apart.
    if (pick.element.isNull()) {
        out.message = "That geometry cannot be referred to, so it cannot be selected.";
        return out;
    }
    const auto object = history_.current().find(pick.object);
    if (!object || object->output() == nullptr) {
        out.message = "That body has not been computed yet.";
        return out;
    }
    const auto shape = object->output()->map.resolve(pick.element);
    const auto wanted = topologyFor(selectionLevel_);
    if (!shape || !wanted) {
        out.message = "That element no longer exists in the model.";
        return out;
    }
    if (shape->type() != *wanted) {
        // Honest about the common case rather than silent: the mesh carries faces and edges, so at
        // Vertex level there is nothing to hit at all, and a click that does nothing without saying
        // why is the exact complaint this seam exists to answer.
        out.message = std::string("Nothing to select at ") + toString(selectionLevel_) +
                      " level here.";
        return out;
    }

    if (!additive) elementSelection_.clear();
    const auto same = [&](const ElementSelection& e) { return e.element == pick.element; };
    const auto it = std::find_if(elementSelection_.begin(), elementSelection_.end(), same);
    if (it != elementSelection_.end()) {
        elementSelection_.erase(it);        // clicking a selected element again deselects it
        out.message = "Deselected";
    } else {
        elementSelection_.push_back({pick.object, pick.element, pick.slot});
        out.message = std::string("Selected ") + toString(selectionLevel_) + " " +
                      pick.element.toString();
    }
    out.changed = true;
    refreshHighlights();
    notifyDocument();
    return out;
}

bool Controller::hoverAt(std::uint32_t x, std::uint32_t y) {
    const Pick pick = pickAt(x, y);
    const std::optional<std::uint32_t> now =
        pick.hit ? std::optional<std::uint32_t>{pick.slot} : std::nullopt;
    if (now == hoveredSlot_) return false;   // the shell repaints on the return value, not per event
    hoveredSlot_ = now;
    refreshHighlights();
    return true;
}

bool Controller::clearHover() {
    if (!hoveredSlot_) return false;
    hoveredSlot_.reset();
    refreshHighlights();
    return true;
}

void Controller::scriptNextPick(std::uint32_t elementSlot, bool valid) {
    if (gpu_ != nullptr) return;
    render::IPicker::Hit hit;
    hit.element = elementSlot;
    hit.valid = valid;
    backend_.picker.setNextHit(hit);
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

const char* toString(Environment e) noexcept {
    switch (e) {
        case Environment::Model:  return "Model";
        case Environment::Sketch: return "Sketch";
    }
    return "Model";
}

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

bool Controller::beginCommand(const std::string& id) {
    // The parameter set per command. A table rather than a virtual per-command class: there will be
    // dozens of commands and almost all of them are two or three numbers, so a class each would be
    // ceremony without benefit. A command that needs more than this gets its own panel later.
    commandParameters_.clear();
    if (id == "feature.extrude") {
        if (selection_.size() != 1) {
            status("Select a sketch to extrude.");
            return false;
        }
        commandParameters_.push_back(
            {"distance", "Distance", CommandParameter::Kind::Length,
             units::format(units::millimetres(10.0), preferences_.displayUnits)});
    } else if (id == "feature.box") {
        for (const auto& [name, label, mm] : {std::tuple{"dx", "Length", 100.0},
                                              std::tuple{"dy", "Width", 60.0},
                                              std::tuple{"dz", "Height", 40.0}}) {
            commandParameters_.push_back(
                {name, label, CommandParameter::Kind::Length,
                 units::format(units::millimetres(mm), preferences_.displayUnits)});
        }
    } else if (id == "feature.cylinder") {
        for (const auto& [name, label, mm] : {std::tuple{"radius", "Radius", 25.0},
                                              std::tuple{"height", "Height", 80.0}}) {
            commandParameters_.push_back(
                {name, label, CommandParameter::Kind::Length,
                 units::format(units::millimetres(mm), preferences_.displayUnits)});
        }
    } else {
        return false;   // no parameters: the shell invokes it directly, as before
    }

    activeCommand_ = id;
    notifyDocument();
    status("Enter values, then OK.");
    return true;
}

bool Controller::setCommandParameter(const std::string& name, const std::string& text) {
    for (auto& p : commandParameters_) {
        if (p.name != name) continue;
        // Validated by PARSING it, not by pattern-matching the text: the unit grammar lives in one
        // place and this must accept exactly what a property field accepts, including "2 in".
        if (p.kind == CommandParameter::Kind::Length) {
            const // A bare number is read in the DISPLAY units, so what you type back matches what you
        // just read. Typing "2" into a field showing inches must mean two inches.
        auto parsed = units::parseLength(text, preferences_.displayUnits);
            if (!parsed) {
                // The old value is kept. A field that blanks itself on a typo loses work the user
                // has already done in the other fields.
                status("Could not read \"" + text + "\" as a length.");
                return false;
            }
        }
        p.value = text;
        notifyDocument();
        return true;
    }
    return false;
}

bool Controller::commitCommand() {
    if (activeCommand_.empty()) return false;
    const std::string id = activeCommand_;

    const auto lengthOf = [this](const char* name) -> double {
        for (const auto& p : commandParameters_) {
            if (p.name != name) continue;
            if (const auto parsed = units::parseLength(p.value, preferences_.displayUnits)) {
                return parsed.value().base();
            }
        }
        return 0.0;
    };

    bool ok = false;
    if (id == "feature.extrude") {
        addExtrude(lengthOf("distance"));
        ok = true;
    } else if (id == "feature.box") {
        addPrimitive("Box", {{"dx", lengthOf("dx")}, {"dy", lengthOf("dy")},
                             {"dz", lengthOf("dz")}});
        ok = true;
    } else if (id == "feature.cylinder") {
        addPrimitive("Cylinder", {{"radius", lengthOf("radius")}, {"height", lengthOf("height")}});
        ok = true;
    }

    // Cleared AFTER the command runs, so a command that inspects the selection still sees it.
    activeCommand_.clear();
    commandParameters_.clear();
    notifyDocument();
    return ok;
}

void Controller::cancelCommand() {
    if (activeCommand_.empty()) return;
    activeCommand_.clear();
    commandParameters_.clear();
    notifyDocument();
    status("Cancelled");
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
    notifyView();
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

bool Controller::sketchClickAt(float x, float y) {
    if (environment_ != Environment::Sketch || !editing_.has_value()) return false;
    if (sketchTool_ == SketchTool::Select) return false;

    const auto point = sketchPointAt(x, y);
    // Refused rather than snapped to something arbitrary. A click that missed the plane -- edge-on,
    // or a grazing angle -- has no sketch coordinate, and inventing one puts geometry where the
    // user cannot see it.
    if (!point) {
        status("That click did not land on the sketch plane.");
        return false;
    }

    if (!sketchPending_) {
        sketchPending_ = *point;
        // Starts at the click, so the band has zero length until the pointer moves rather than
        // flicking from wherever the last shape ended.
        sketchHover_ = *point;
        status(sketchTool_ == SketchTool::Line ? "Line: click the end point"
                                               : "Circle: click to set the radius");
        notifyView();
        return true;
    }

    const std::array<double, 2> first = *sketchPending_;
    sketchPending_.reset();
    sketchHover_.reset();

    if (sketchTool_ == SketchTool::Line) {
        const double dx = (*point)[0] - first[0];
        const double dy = (*point)[1] - first[1];
        // A zero-length line is a double-click, not a request. It would be refused by addLine
        // anyway on the next solve, but silently: better to say nothing was drawn.
        if (std::abs(dx) < 1e-9 && std::abs(dy) < 1e-9) {
            status("A line needs two different points.");
            return false;
        }
        editing_->addLine(first[0], first[1], (*point)[0], (*point)[1]);
    } else {
        const double dx = (*point)[0] - first[0];
        const double dy = (*point)[1] - first[1];
        const double radius = std::sqrt(dx * dx + dy * dy);
        if (radius < 1e-9) {
            status("A circle needs a radius.");
            return false;
        }
        editing_->addCircle(first[0], first[1], radius);
    }

    // Every mutation is followed by a solve, because a sketch that does not follow its constraints
    // while you draw is not a sketch -- it is a drawing that will jump when you finally solve it.
    lastSketchSolve_ = editing_->solve();
    status(lastSketchSolve_.message);
    pushSketchOverlay();
    notifyDocument();
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

void Controller::pushSketchOverlay() {
    if (!scene_) return;
    // Cleared when leaving the sketch, or the finished sketch would be drawn twice: once as the
    // overlay and once as the feature's own edges, at slightly different depths.
    const auto lines = sketchOverlayVertices();
    scene_->setSketchOverlay(lines, sketchOverlayRevision());
    const auto preview = sketchPreviewVertices();
    scene_->setSketchPreview(preview, sketchPreviewRevision());
}

void Controller::restoreCameraAfterSketch() {
    if (!cameraBeforeSketch_) return;
    camera_ = *cameraBeforeSketch_;
    cameraBeforeSketch_.reset();
    notifyView();
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

ObjectId Controller::addSketchOnFace(ObjectId body, const std::string& face) {
    const auto owner = history_.current().find(body);
    if (!owner) {
        status("That body is not in this document.");
        return {};
    }
    if (face.empty()) {
        status("A sketch on a face needs a face.");
        return {};
    }

    // Empty. Unlike addSketch's seeded rectangle, a sketch the user placed deliberately on a face
    // is one they are about to draw in -- handing them a 40 x 25 rectangle to delete first is the
    // kind of "helpful" default that only ever costs a step.
    sketch::Sketch sk;
    sketch::SketchPlane placement;
    placement.kind = sketch::SketchPlane::Kind::Face;
    placement.face = face;
    sk.setPlacement(placement);

    auto [next, id] = history_.current().add("Sketch");
    const auto object = next.find(id);
    // No "plane" property: this sketch has no global plane, and an extrude built from it takes its
    // direction by measuring the profile instead.
    auto updated = object->withProperty("sketch", sk.serialize()).withProperty("body", body);
    next = next.replace(std::make_shared<const document::ObjectData>(std::move(updated)));
    history_.commit(std::move(next), "Sketch");

    selection_.clear();
    selection_.push_back(id);
    refresh();
    status("Added Sketch on a face");
    return id;
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

    // Carried across ONLY when the sketch actually has one. A sketch placed on a face has no global
    // plane, and defaulting to XY here used to hand the extrude a direction lying in the profile's
    // own plane -- which sweeps to a zero-volume sheet. With the property absent, computeExtrude
    // measures the profile's normal instead, which is the right answer for any face.
    std::optional<std::int64_t> plane;
    if (const auto* stored = object->find("plane")) {
        if (const auto* v = std::get_if<std::int64_t>(stored)) plane = *v;
    }

    auto [next, id] = history_.current().add("Extrude");
    const auto created = next.find(id);
    auto updated = created->withProperty("a_profile", profile)
                       .withProperty("distance", units::millimetres(millimetres));
    if (plane) updated = updated.withProperty("plane", *plane);
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
    // Picked edges first. This is what edge selection was for: before it existed the only honest
    // thing to do was every edge of the body, and the status line said so.
    ObjectId target;
    std::vector<naming::ElementName> edges;
    bool wholeBody = false;

    if (!elementSelection_.empty() && selectionLevel_ == SelectionLevel::Edge) {
        // All from one object. A fillet takes a base shape and edges OF it, so edges from two bodies
        // is not a feature with a strange input -- it is two features, and guessing which one the
        // user meant would silently drop half the selection.
        target = elementSelection_.front().object;
        for (const ElementSelection& picked : elementSelection_) {
            if (picked.object != target) {
                status("Select edges on one body at a time.");
                return;
            }
            edges.push_back(picked.element);
        }
    } else if (selection_.size() == 1) {
        target = selection_.front();
        edges = edgesOf(target);
        wholeBody = true;
    } else {
        status("Select a body, or the edges to " + label + ".");
        return;
    }

    if (edges.empty()) {
        status("Nothing to " + label + ": that feature has no edges yet.");
        return;
    }

    // Counted BEFORE the std::move below. Reading edges.size() after moving the vector into the
    // property reports zero -- which is how the status line came to claim "applied to all 0 edges".
    const std::size_t edgeCount = edges.size();

    auto [next, id] = history_.current().add(type);
    const auto object = next.find(id);
    auto updated = object->withProperty("a_base", target)
                       .withProperty(sizeProperty, units::millimetres(millimetres))
                       .withProperty("edges", std::move(edges));
    next = next.replace(std::make_shared<const document::ObjectData>(std::move(updated)));
    history_.commit(std::move(next), label);
    selection_.clear();
    // The picked edges belonged to the OLD feature's output, which this feature consumed. Leaving
    // them selected would mark geometry that no longer exists.
    elementSelection_.clear();
    selection_.push_back(id);
    refresh();

    // Report what actually happened. A fillet that silently failed on 3 of 12 edges is the kind
    // of thing the engine records and the user would otherwise never learn: partial failure is a
    // feature of the recompute engine, so it has to be a feature of the status line too.
    const auto result = history_.current().find(id);
    if (result && result->output() == nullptr) {
        status(label + " failed — see the feature's error in the browser.");
    } else if (wholeBody) {
        status(label + " applied to all " + std::to_string(edgeCount) +
               " edges of the body. Select edges first to " + label + " only those.");
    } else {
        status(label + " applied to " + std::to_string(edgeCount) +
               (edgeCount == 1 ? " edge." : " selected edges."));
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
    commands_.push_back({"sketch.finish", "Finish Sketch",
                         "Apply the sketch and return to the model", "sketch-finish",
                         [this](const CommandContext&) {
                             return environment_ == Environment::Sketch;
                         },
                         [this] { finishSketch(); }});
    commands_.push_back({"sketch.cancel", "Cancel Sketch",
                         "Discard changes and return to the model", "delete",
                         [this](const CommandContext&) {
                             return environment_ == Environment::Sketch;
                         },
                         [this] { cancelSketch(); }});
    commands_.push_back({"sketch.edit", "Edit Sketch",
                         "Edit the selected sketch's geometry", "sketch-edit",
                         [this](const CommandContext& c) {
                             if (c.selectedObjects != 1) return false;
                             const auto o = history_.current().find(selection_.front());
                             return o != nullptr && o->type() == "Sketch";
                         },
                         [this] {
                             if (selection_.size() == 1) editSketch(selection_.front());
                         }});

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
                         [this] { beginSketch(); }});
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
        // Either input: picked edges, or a single body whose edges we would take wholesale. A
        // greyed-out Fillet with three edges selected would read as the selection not counting.
        if (!elementSelection_.empty() && selectionLevel_ == SelectionLevel::Edge) return true;
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
