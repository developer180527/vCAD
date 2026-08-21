/// The command surface a shell drives: the catalogue, parameterised commands, and the sketch
/// constraint commands that share their arity rules.
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
    } else if (id == "feature.hole") {
        if (elementSelection_.size() != 1 || selectionLevel_ != SelectionLevel::Face) {
            status("Select one flat face to put the hole in.");
            return false;
        }
        // 8 mm and 10 mm: an ordinary clearance hole rather than a round number that fits nothing.
        // The depth is what a user changes most, so it is second and therefore focused after a Tab.
        for (const auto& [name, label, mm] : {std::tuple{"diameter", "Diameter", 8.0},
                                              std::tuple{"depth", "Depth", 10.0}}) {
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
    } else if (id == "feature.hole") {
        addHole(lengthOf("diameter"), lengthOf("depth"));
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

    // Hole. Enabled on a single FACE, which is its entire input -- the position and the direction
    // both come from the face, so there is nothing to pick afterwards and nothing to guess.
    //
    // It has computed correctly since the day it was written and no user could reach it, because
    // nothing added it here and both shells build their tools from this catalogue. That is the
    // cheapest kind of gap there is, and Hole is the most-used feature in mechanical CAD.
    commands_.push_back({"feature.hole", "Hole", "Drill a hole into the selected face", "hole",
                         [this](const CommandContext& c) {
                             return c.selectedElements == 1
                                    && selectionLevel_ == SelectionLevel::Face;
                         },
                         [this] { addHole(8.0, 10.0); }});

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
