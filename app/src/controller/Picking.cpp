/// Turning pixels into selection: the GPU pick, what a click resolves to at each selection
/// level, hover, and the highlight set that follows from both.
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
        case Controller::SelectionLevel::Auto:   return "Auto";
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
        // Auto has no fixed answer: what it resolves to is decided per pick, by what was hit.
        case Controller::SelectionLevel::Auto: return std::nullopt;
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
    return applyPick(pickAt(x, y), additive);
}

/// What a click or a tap DOES with the thing it found.
///
/// Split out of clickAt when tapAt arrived, so the two differ only in how they decide WHAT was
/// pointed at — one pixel for a mouse, an aperture for a finger — and not at all in what pointing
/// at it means. Two copies of this body is how the desktop and the iPad would end up disagreeing
/// about whether a second tap deselects.
Controller::ClickResult Controller::applyPick(const Pick& pick, bool additive) {
    ClickResult out;

    if (!pick.hit) {
        // Empty space. Clearing is what every CAD application does, and it is what keeps the tree
        // and the viewport agreeing about what is selected.
        if (additive) return out;
        const bool had = !selection_.empty() || !elementSelection_.empty();
        selection_.clear();
        elementSelection_.clear();
        // A click on empty space is also "I am not pointing at that face any more".
        lastPicked_.reset();
        out.changed = had;
        if (had) {
            refreshHighlights();
            notifyDocument();
        }
        return out;
    }

    out.hit = true;

    // Remembered BEFORE the level branch, so it is recorded even when the click selects a body and
    // discards the element. This is what lets "click a face, press Start Sketch" work without the
    // user first switching the selection filter to Face.
    if (!pick.object.isNull() && !pick.element.isNull()) {
        lastPicked_ = ElementSelection{pick.object, pick.element, pick.slot};
    }

    // Auto selects the ELEMENT that was hit — the vertex, edge or face the ranking chose — rather
    // than asking the user to declare a level first. Body remains reachable through the level
    // parameter, which is what a double tap uses.
    if (selectionLevel_ == SelectionLevel::Auto && !pick.element.isNull() && !pick.object.isNull()) {
        const auto object = history_.current().find(pick.object);
        if (object && object->output() != nullptr) {
            if (const auto shape = object->output()->map.resolve(pick.element)) {
                if (!additive) {
                    elementSelection_.clear();
                    // AND the object selection. Leaving it made a replaced selection only look
                    // replaced: a body selected by an earlier double tap stayed in selection_,
                    // which is what commands like Fillet act on — so the next Fillet rounded a
                    // body the user believed they had deselected.
                    selection_.clear();
                }
                const auto same = [&](const ElementSelection& e) { return e.element == pick.element; };
                const auto it = std::find_if(elementSelection_.begin(), elementSelection_.end(), same);
                if (it != elementSelection_.end()) {
                    elementSelection_.erase(it);
                    out.message = "Deselected";
                } else {
                    elementSelection_.push_back({pick.object, pick.element, pick.slot});
                    out.message = std::string("Selected ") + kernel::toString(shape->type());
                }
                out.changed = true;
                refreshHighlights();
                notifyDocument();
                return out;
            }
        }
    }

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

namespace {

/// The ranking's notion of what a thing is, from the kernel's.
PickKind kindOf(kernel::ShapeType type) {
    switch (type) {
        case kernel::ShapeType::Vertex: return PickKind::Vertex;
        case kernel::ShapeType::Edge:   return PickKind::Edge;
        case kernel::ShapeType::Face:   return PickKind::Face;
        default:                        return PickKind::Unknown;
    }
}

const char* nameOf(PickKind kind) {
    switch (kind) {
        case PickKind::Vertex: return "Vertex";
        case PickKind::Edge:   return "Edge";
        case PickKind::Face:   return "Face";
        case PickKind::Unknown: break;
    }
    return "Element";
}

}   // namespace

std::vector<Controller::Candidate> Controller::candidatesAt(std::uint32_t x, std::uint32_t y,
                                                            std::uint32_t radiusPixels) {
    std::vector<Candidate> out;
    if (active_.picker == nullptr) return out;

    std::vector<render::IPicker::ApertureHit> hits;
    active_.picker->pickAperture(scene_->frame(), x, y, radiusPixels, hits);
    if (hits.empty()) return out;

    // Ranked in the pure type and then re-joined, rather than sorting `Candidate` directly. The
    // rule is worth being able to test without a document, a scene or a GPU — those are exactly
    // the things that make the interesting cases (an edge one pixel inside a face) unreasonable
    // to arrange in a test.
    std::vector<PickCandidate> ranked;
    ranked.reserve(hits.size());
    std::vector<Candidate> resolved;
    resolved.reserve(hits.size());

    for (const auto& hit : hits) {
        Candidate c;
        c.slot = hit.element;
        c.distanceSq = static_cast<std::uint64_t>(hit.dx) * hit.dx
                       + static_cast<std::uint64_t>(hit.dy) * hit.dy;

        render::IPicker::Hit raw;
        raw.element = hit.element;
        raw.valid = true;
        if (const auto name = scene_->resolve(raw)) c.element = *name;
        if (const auto owner = scene_->objectOf(hit.element)) c.object = *owner;

        // The kind comes from the RESOLVED TOPOLOGY, never from the name. An edge and the face
        // bounding it can share a feature and an operation, so their names do not distinguish
        // them — the same reason clickAt resolves the shape before honouring a selection level.
        std::string ownerLabel;
        if (const auto object = history_.current().find(c.object)) {
            ownerLabel = object->label();
            if (object->output() != nullptr && !c.element.isNull()) {
                if (const auto shape = object->output()->map.resolve(c.element)) {
                    c.kind = kindOf(shape->type());
                }
            }
        }
        c.label = ownerLabel.empty() ? nameOf(c.kind) : ownerLabel + " · " + nameOf(c.kind);

        ranked.push_back(PickCandidate{c.slot, c.kind, c.distanceSq, c.depth});
        resolved.push_back(std::move(c));
    }

    // A level RESTRICTS rather than reorders — it decides what a pick resolves to. Body keeps
    // everything, because a body is reached through whichever element was hit.
    if (const auto wanted = topologyFor(selectionLevel_)) {
        restrictToKind(ranked, kindOf(*wanted));
    }
    rankCandidates(ranked);

    out.reserve(ranked.size());
    for (const auto& r : ranked) {
        const auto it = std::find_if(resolved.begin(), resolved.end(),
                                     [&](const Candidate& c) { return c.slot == r.slot; });
        if (it != resolved.end()) out.push_back(*it);
    }
    return out;
}

Controller::ClickResult Controller::tapAt(std::uint32_t x, std::uint32_t y,
                                          std::uint32_t radiusPixels, bool additive,
                                          SelectionLevel level) {
    // Swapped for the duration of the call rather than duplicating applyPick. The level is what
    // "resolves to" MEANS, and threading it through every function below would be the same
    // substitution written four more times.
    const SelectionLevel previous = selectionLevel_;
    selectionLevel_ = level;
    auto out = tapAt(x, y, radiusPixels, additive);
    selectionLevel_ = previous;
    return out;
}

Controller::ClickResult Controller::tapAt(std::uint32_t x, std::uint32_t y,
                                          std::uint32_t radiusPixels, bool additive) {
    const auto candidates = candidatesAt(x, y, radiusPixels);

    Pick pick;
    if (!candidates.empty()) {
        pick.hit = true;
        pick.object = candidates.front().object;
        pick.element = candidates.front().element;
        pick.slot = candidates.front().slot;
        pick.depth = candidates.front().depth;
    }
    auto out = applyPick(pick, additive);
    out.candidates = candidates.size();
    // Said out loud when the tap was ambiguous, because that is when a shell should be offering
    // the rest of the list — and when a user who got the wrong thing needs to know there was a
    // choice rather than concluding the picker is inaccurate.
    if (candidates.size() > 1 && out.hit) {
        out.message += " (" + std::to_string(candidates.size() - 1) + " more here)";
    }
    return out;
}

bool Controller::hoverAt(std::uint32_t x, std::uint32_t y) { return hoverAt(x, y, 0); }

bool Controller::hoverAt(std::uint32_t x, std::uint32_t y, std::uint32_t radiusPixels) {
    // The SAME aperture and the SAME ranking a click uses.
    //
    // Hover and click must agree, and with a one-pixel hover they could not: the pointer would
    // pre-highlight the face while the click a moment later selected the edge crossing it, because
    // only one of the two had a tolerance. Pre-highlight exists to answer "what will this click
    // take" — an answer arrived at by a different rule is not an answer.
    std::optional<std::uint32_t> now;
    if (radiusPixels == 0) {
        const Pick pick = pickAt(x, y);
        if (pick.hit) now = pick.slot;
    } else {
        const auto candidates = candidatesAt(x, y, radiusPixels);
        if (!candidates.empty()) now = candidates.front().slot;
    }
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

void Controller::scriptPickForTest(ObjectId id, const naming::ElementName& element) {
    lastPicked_ = ElementSelection{id, element, 0};
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

}  // namespace cad::app
