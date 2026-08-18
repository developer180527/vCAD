/// Creating features: primitives, sketches, extrudes, booleans and edge features.
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

}  // namespace cad::app
