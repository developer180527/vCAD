/// Creating features: primitives, sketches, extrudes, booleans and edge features.
///
/// Split out of Controller.cpp, which had reached 2574 lines. The class is unchanged --
/// these are the same methods in the same order, moved verbatim into a file named for what
/// they do, so the system can be read one concern at a time.

#include "Internal.h"

#include <numbers>

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
    // EMPTY. It used to seed a fully constrained 40 x 25 rectangle, with a comment saying "until
    // the sketch editor exists" — and the sketch editor exists now. The seed computed into a face,
    // so the renderer drew it as a square sheet that appeared from nowhere the moment a user pressed
    // Start Sketch, and the first thing they had to do was delete geometry they never asked for.
    sketch::Sketch sk(sketch::Plane::XY);

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
    status("Added Sketch — draw on it with the Line and Circle tools");
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

void Controller::addRevolve(double degrees) {
    // An EDGE of the sketch, which is both the axis and the way the profile is chosen.
    //
    // `computeRevolve` resolves the axis name in the PROFILE's own element map, so the axis has to
    // be an edge of the sketch being revolved -- not of some other body. Selecting the edge
    // therefore identifies both inputs at once, and there is nothing left to guess.
    // Asked of the FEATURE'S declaration, so this guard and the button's enablement cannot come to
    // disagree -- which is exactly how Revolve became impossible to invoke while computing
    // correctly. The message comes from there too, so there is one wording to keep right.
    if (const auto shortfall = selectionShortfall("Revolve"); !shortfall.empty()) {
        status(shortfall);
        return;
    }
    const auto selected = selectionByKind();
    const ElementSelection picked = selected.edges.front();

    const auto object = history_.current().find(picked.object);
    if (!object || object->output() == nullptr) {
        status("That sketch has not been computed yet.");
        return;
    }
    if (object->type() != "Sketch") {
        status("Revolve turns a sketch about one of its own edges.");
        return;
    }

    // Refused before a feature exists, for the same reason Hole refuses a curved face: the compute
    // would reject it too, but only after leaving a failed row in the browser to find and delete.
    const auto edge = object->output()->map.resolve(picked.element);
    if (!edge) {
        status("That edge no longer exists in the model.");
        return;
    }
    if (const auto line = kernel::lineOf(*edge); !line) {
        // The kernel measured the curve and knows why. A second judgement here could disagree.
        status(line.error().message);
        return;
    }

    auto [next, id] = history_.current().add("Revolve");
    const auto created = next.find(id);
    auto updated = created->withProperty("a_profile", picked.object)
                       .withProperty("axis", picked.element)
                       .withProperty("angle", units::Angle::fromBase(
                                                  degrees * std::numbers::pi / 180.0));
    next = next.replace(std::make_shared<const document::ObjectData>(std::move(updated)));
    history_.commit(std::move(next), "Revolve");

    selection_.clear();
    // The axis belonged to the profile this revolve consumed.
    elementSelection_.clear();
    selection_.push_back(id);
    refresh();

    const auto result = history_.current().find(id);
    if (result && result->output() == nullptr) {
        status("Revolve failed — see the feature's error in the browser.");
    } else {
        status("Revolved " + units::format(units::Angle::fromBase(
                                               degrees * std::numbers::pi / 180.0)));
    }
}

void Controller::addTranslate(double dxMm, double dyMm, double dzMm) {
    if (selection_.size() != 1) {
        status("Select one body to move.");
        return;
    }
    const ObjectId target = selection_.front();
    const auto object = history_.current().find(target);
    if (!object || object->output() == nullptr) {
        status("That feature has not been computed yet.");
        return;
    }

    // A move of nothing is not a move. Adding the feature anyway would put a row in the browser
    // that changes the part not at all, which the user then has to recognise and delete.
    if (dxMm == 0.0 && dyMm == 0.0 && dzMm == 0.0) {
        status("Enter a distance to move by.");
        return;
    }

    auto [next, id] = history_.current().add("Translate");
    const auto created = next.find(id);
    auto updated = created->withProperty("a_base", target)
                       .withProperty("dx", units::millimetres(dxMm))
                       .withProperty("dy", units::millimetres(dyMm))
                       .withProperty("dz", units::millimetres(dzMm));
    next = next.replace(std::make_shared<const document::ObjectData>(std::move(updated)));
    history_.commit(std::move(next), "Move");

    selection_.clear();
    elementSelection_.clear();
    selection_.push_back(id);
    refresh();

    const auto result = history_.current().find(id);
    if (result && result->output() == nullptr) {
        status("Move failed — see the feature's error in the browser.");
    } else {
        const auto mm = [this](double v) {
            return units::format(units::millimetres(v), preferences_.displayUnits);
        };
        status("Moved by " + mm(dxMm) + ", " + mm(dyMm) + ", " + mm(dzMm));
    }
}

void Controller::addHole(double diameterMm, double depthMm) {
    // A FACE, singular. The feature drills perpendicular to one flat face at its centre, so "which
    // face" is the whole input and two of them is two holes -- which is a reasonable thing to want
    // and not a reasonable thing to guess.
    if (const auto shortfall = selectionShortfall("Hole"); !shortfall.empty()) {
        status(shortfall);
        return;
    }
    const auto selected = selectionByKind();
    const ElementSelection picked = selected.faces.front();

    // Refused HERE, before a feature exists, rather than by the compute.
    //
    // computeHole would reject a curved face too, but only after the feature had been added to the
    // document -- leaving a failed row in the browser that the user has to find and delete. The
    // shell knows the face already, so it can decline without leaving wreckage.
    const auto object = history_.current().find(picked.object);
    if (!object || object->output() == nullptr) {
        status("That body has not been computed yet.");
        return;
    }
    const auto shape = object->output()->map.resolve(picked.element);
    if (!shape) {
        status("That face no longer exists in the model.");
        return;
    }
    if (const auto plane = kernel::planeOf(*shape); !plane) {
        // The kernel's own words: it measured the surface and knows why. Repeating that judgement
        // here would be a second copy of it, free to disagree.
        status(plane.error().message);
        return;
    }

    auto [next, id] = history_.current().add("Hole");
    const auto created = next.find(id);
    auto updated = created->withProperty("a_base", picked.object)
                       .withProperty("face", picked.element)
                       .withProperty("diameter", units::millimetres(diameterMm))
                       .withProperty("depth", units::millimetres(depthMm));
    next = next.replace(std::make_shared<const document::ObjectData>(std::move(updated)));
    history_.commit(std::move(next), "Hole");

    selection_.clear();
    // The face belonged to the body this hole consumed, so keeping it selected would mark geometry
    // that no longer exists -- the same reason the edge features clear theirs.
    elementSelection_.clear();
    selection_.push_back(id);
    refresh();

    const auto result = history_.current().find(id);
    if (result && result->output() == nullptr) {
        status("Hole failed — see the feature's error in the browser.");
    } else {
        status("Hole " + units::format(units::millimetres(diameterMm), preferences_.displayUnits)
               + " across, " + units::format(units::millimetres(depthMm), preferences_.displayUnits)
               + " deep");
    }
}

void Controller::addEdgeFeature(const std::string& type, const std::string& label,
                                const std::string& sizeProperty, double millimetres) {
    // Picked edges first. This is what edge selection was for: before it existed the only honest
    // thing to do was every edge of the body, and the status line said so.
    ObjectId target;
    std::vector<naming::ElementName> edges;
    bool wholeBody = false;

    // Decided by WHAT IS SELECTED — one question, asked in one place, by every feature that takes
    // geometry. See Controller::selectionByKind for the three bugs that came of each of them
    // working it out separately.
    const auto selected = selectionByKind();

    if (!selected.edges.empty()) {
        // All from one object. A fillet takes a base shape and edges OF it, so edges from two bodies
        // is not a feature with a strange input -- it is two features, and guessing which one the
        // user meant would silently drop half the selection.
        if (!selected.oneOwner()) {
            status("Select edges on one body at a time.");
            return;
        }
        target = selected.owner();
        for (const ElementSelection& picked : selected.edges) edges.push_back(picked.element);
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

}  // namespace cad::app
