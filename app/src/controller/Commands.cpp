/// The command surface a shell drives: the catalogue, parameterised commands, and the sketch
/// constraint commands that share their arity rules.
///
/// Split out of Controller.cpp, which had reached 2574 lines. The class is unchanged --
/// these are the same methods in the same order, moved verbatim into a file named for what
/// they do, so the system can be read one concern at a time.

#include "Internal.h"

#include "cad/document/Parameters.h"
#include "cad/expr/Expression.h"

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
#include <unordered_map>
#include <chrono>

namespace cad::app {

/// The feature type a command id creates, or empty for a command that creates no feature.
///
/// The one place the two vocabularies meet. Commands are named for what the USER does
/// ("feature.hole"); features are named for what they ARE ("Hole"), because that name is written
/// into saved documents and cannot follow a menu rename.
std::string Controller::featureTypeOf(const std::string& commandId) {
    static const std::unordered_map<std::string, std::string> kTypes{
        {"feature.box", "Box"},         {"feature.cylinder", "Cylinder"},
        {"feature.extrude", "Extrude"}, {"feature.revolve", "Revolve"},
        {"feature.hole", "Hole"},       {"feature.translate", "Translate"},
        {"feature.fillet", "Fillet"},   {"feature.chamfer", "Chamfer"},
        {"feature.cut", "Cut"},         {"feature.fuse", "Fuse"},
        {"feature.common", "Common"},
        {"feature.mirror", "Mirror"},   {"feature.pattern", "Pattern"},
    };
    const auto found = kTypes.find(commandId);
    return found == kTypes.end() ? std::string{} : found->second;
}

std::string Controller::selectionShortfall(const std::string& featureType) const {
    const recompute::FeatureType* type = registry_.find(featureType);
    if (type == nullptr) return "That feature is not installed.";
    const auto& inputs = type->inputs;
    if (inputs.accepts.empty()) return {};   // a primitive needs nothing selected

    const auto selected = selectionByKind();
    const auto ownerOf = [this](document::ObjectId id) -> std::string {
        const auto object = history_.current().find(id);
        return object ? object->type() : std::string{};
    };

    // Counted by WHAT IS SELECTED, never by the selection level. The level says what a click
    // resolves to; it does not describe what is already selected, and under Auto it is neither
    // Face nor Edge. That confusion is what made Hole and Revolve impossible to invoke.
    const auto satisfies = [&](const recompute::FeatureInputs::Requirement& need) {
        using Of = recompute::FeatureInputs::Requirement::Of;
        std::size_t count = 0;
        bool ownersMatch = true;

        const auto tally = [&](const auto& picks) {
            count = picks.size();
            for (const auto& pick : picks) {
                if (!need.ownerType.empty() && ownerOf(pick.object) != need.ownerType) {
                    ownersMatch = false;
                }
            }
        };
        switch (need.of) {
            case Of::Face:   tally(selected.faces); break;
            case Of::Edge:   tally(selected.edges); break;
            case Of::Vertex: tally(selected.vertices); break;
            case Of::Object:
                count = selection_.size();
                for (const auto& id : selection_) {
                    if (!need.ownerType.empty() && ownerOf(id) != need.ownerType) {
                        ownersMatch = false;
                    }
                }
                break;
        }
        if (!ownersMatch || count < need.least) return false;
        return need.most == 0 || count <= need.most;
    };

    for (const auto& need : inputs.accepts) {
        if (satisfies(need)) return {};
    }
    return inputs.prompt.empty() ? "That is not the right selection for this." : inputs.prompt;
}

bool Controller::beginCommand(const std::string& id) {
    // Built from the FEATURE'S OWN DECLARATION, not from a table here. The table this replaces
    // listed each feature's selection rule, its refusal message and its fields -- all of which the
    // feature also stated elsewhere, and all of which drifted. See recompute/FeatureInputs.h.
    commandParameters_.clear();

    const std::string type = featureTypeOf(id);
    const recompute::FeatureType* feature = type.empty() ? nullptr : registry_.find(type);
    if (feature == nullptr || feature->inputs.values.empty()) {
        return false;   // no values to type: the shell invokes it directly, as before
    }

    if (const auto shortfall = selectionShortfall(type); !shortfall.empty()) {
        status(shortfall);
        return false;
    }

    for (const auto& value : feature->inputs.values) {
        CommandParameter parameter;
        parameter.name = value.name;
        parameter.label = value.label;
        using Kind = recompute::FeatureInputs::Value::Kind;
        switch (value.kind) {
            case Kind::Angle:
                parameter.kind = CommandParameter::Kind::Angle;
                parameter.value = units::format(units::Angle::fromBase(value.base));
                break;
            case Kind::Count:
                parameter.kind = CommandParameter::Kind::Integer;
                parameter.value = std::to_string(static_cast<long long>(value.base));
                break;
            case Kind::Number:
                parameter.kind = CommandParameter::Kind::Real;
                parameter.value = document::toString(document::PropertyValue{value.base});
                break;
            case Kind::Bool:
                parameter.kind = CommandParameter::Kind::Bool;
                parameter.value = value.base != 0.0 ? "true" : "false";
                break;
            case Kind::Length:
            default:
                parameter.kind = CommandParameter::Kind::Length;
                // Shown in the user's units. The declaration holds base units precisely so that a
                // preference cannot change what the feature means, only how it is displayed.
                parameter.value =
                    units::format(units::Length::fromBase(value.base), preferences_.displayUnits);
                break;
        }
        commandParameters_.push_back(std::move(parameter));
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
            // A bare number is read in the DISPLAY units, so what you type back matches what you
            // just read: typing "2" into a field showing inches must mean two inches. Names resolve
            // against the document's parameters, so a new feature can be dimensioned in terms of
            // them at the moment it is created rather than only afterwards.
            const auto resolver = document::resolveParameters(history_.current()).resolver();
            const auto parsed = expr::evaluateLength(text, preferences_.displayUnits, resolver);
            if (!parsed) {
                // The old value is kept. A field that blanks itself on a typo loses work the user
                // has already done in the other fields.
                status(parsed.error().message);
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
            const auto resolver = document::resolveParameters(history_.current()).resolver();
            if (const auto parsed =
                    expr::evaluateLength(p.value, preferences_.displayUnits, resolver)) {
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
    } else if (id == "feature.revolve") {
        // Parsed as an ANGLE, not a length: "90" is degrees, "1.57rad" is not, and units::parseAngle
        // is the one place that knows which is which.
        double degrees = 360.0;
        for (const auto& p : commandParameters_) {
            if (p.name != "angle") continue;
            if (auto parsed = units::parseAngle(p.value)) {
                degrees = parsed.value().base() * 180.0 / std::numbers::pi;
            }
        }
        addRevolve(degrees);
        ok = true;
    } else if (id == "feature.pattern") {
        // The count is an INTEGER, not a length: "3" is three copies, and running it through
        // lengthOf would parse it as 3 mm and then round it back to 3 by accident.
        std::int64_t copies = 1;
        for (const auto& p : commandParameters_) {
            if (p.name != "count") continue;
            try {
                copies = std::stoll(p.value);
            } catch (const std::exception&) {
                copies = 1;   // unparseable: the compute refuses a count below one and says so
            }
        }
        addPattern(copies, lengthOf("dx"), lengthOf("dy"), lengthOf("dz"));
        ok = true;
    } else if (id == "feature.translate") {
        addTranslate(lengthOf("dx"), lengthOf("dy"), lengthOf("dz"));
        ok = true;
    }

    // Carry any FORMULA the user typed onto the feature that was just created.
    //
    // Without this, typing `width * 2` into the Box width field would work exactly once: the box
    // would be the right size and permanently disconnected from `width`. That is the silent
    // failure the whole expression layer exists to prevent, and it would appear at the one moment
    // a user is most likely to reach for a parameter -- while creating the feature.
    //
    // Safe because every command parameter is named after the property the feature stores it in;
    // a mismatch simply finds nothing and changes nothing.
    if (ok && !selection_.empty()) {
        const auto id = selection_.front();
        if (const auto object = history_.current().find(id)) {
            auto updated = *object;
            bool touched = false;
            for (const auto& p : commandParameters_) {
                const auto* property = updated.find(p.name);
                if (property == nullptr) continue;
                const auto type = document::typeOf(*property);
                if (isPlainQuantity(p.value, type, preferences_.displayUnits)) continue;
                updated = updated.withExpression(p.name, *property, p.value,
                                                 preferences_.displayUnits);
                touched = true;
            }
            if (touched) {
                // AMENDED onto the create, not committed after it. Creating one box must be one
                // undo step; a second commit here would make the user press undo twice, with the
                // first press appearing to do nothing.
                history_.replaceCurrent(history_.current().replace(
                    std::make_shared<const document::ObjectData>(std::move(updated))));
            }
        }
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

    // A declared default, in base units. The direct-invoke path used to repeat every number the
    // declaration already held -- Box 100/60/40, Hole 8/10, Fillet 5, Chamfer 3, Extrude 10 -- so
    // the two agreed only as long as nobody edited one of them.
    //
    // Translate is the deliberate exception and stays a literal at its call site: its declared
    // default is zero, because a move must not move anything the moment its panel opens, while its
    // direct invoke nudges by 10mm so that pressing the button does something visible.
    const auto declared = [this](const char* type, const char* value) {
        const recompute::FeatureType* feature = registry_.find(type);
        if (feature == nullptr) return 0.0;
        for (const auto& v : feature->inputs.values) {
            if (v.name == value) return v.base;
        }
        return 0.0;
    };

    // Enablement DERIVED from the feature's declared inputs. One rule, read from the feature,
    // instead of a predicate per command written beside it and drifting from it.
    const auto needs = [this](const char* commandId) {
        const std::string type = featureTypeOf(commandId);
        return [this, type](const CommandContext&) { return selectionShortfall(type).empty(); };
    };

    commands_.push_back({"feature.box", "Box", "Create a rectangular block", "box", always,
                         [this, declared] {
                             addPrimitive("Box", {{"dx", declared("Box", "dx")},
                                                  {"dy", declared("Box", "dy")},
                                                  {"dz", declared("Box", "dz")}});
                         }});
    commands_.push_back({"feature.cylinder", "Cylinder", "Create a cylinder", "cylinder", always,
                         [this, declared] {
                             addPrimitive("Cylinder",
                                          {{"radius", declared("Cylinder", "radius")},
                                           {"height", declared("Cylinder", "height")}});
                         }});

    // Booleans. Inventor exposes all three as modes of one "Combine" command; we register them
    // separately so each is reachable now, and the mode selector can fold them together once the
    // non-modal command surface exists (DESKTOP_UX 3.2).
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
                         needs("feature.extrude"),
                         [this, declared] { addExtrude(declared("Extrude", "distance")); }});

    commands_.push_back({"feature.cut", "Cut", "Subtract the second selection from the first",
                         "cut", needs("feature.cut"), [this] { addBoolean("Cut", "Cut"); }});
    commands_.push_back({"feature.fuse", "Join", "Merge the selected bodies into one", "combine",
                         needs("feature.fuse"), [this] { addBoolean("Fuse", "Join"); }});
    commands_.push_back({"feature.common", "Intersect",
                         "Keep only the volume the selected bodies share", "combine",
                         needs("feature.common"),
                         [this] { addBoolean("Common", "Intersect"); }});

    // Edge features. Enabled on a single selection that HAS edges — asking for a fillet on a
    // feature with no computed output should not offer itself as available.
    // Fillet and Chamfer keep a SUPPLEMENTARY predicate on top of the declaration. "Picked edges
    // or one body" is declared; "and that body must actually have edges" is a question about
    // geometry rather than about the selection, and inventing a way to declare it would be a
    // larger thing to get wrong than this exception is.
    //
    // Takes the command id. Hardcoding "feature.fillet" here worked only because the two
    // declarations are identical today -- the moment Chamfer's requirement diverges it would
    // silently enforce Fillet's, and nothing would notice. That is the exact failure this whole
    // change exists to remove, reintroduced in the helper that removes it.
    const auto oneWithEdges = [this, needs](const char* commandId) {
        return [this, needs, commandId](const CommandContext& c) {
            if (!needs(commandId)(c)) return false;
            if (c.selectedEdges > 0) return true;
            return c.selectedObjects == 1 && !edgesOf(selection_.front()).empty();
        };
    };
    commands_.push_back({"feature.fillet", "Fillet", "Round every edge of the selected body",
                         "fillet", oneWithEdges("feature.fillet"),
                         [this, declared] {
                             addEdgeFeature("Fillet", "Fillet", "radius",
                                            declared("Fillet", "radius"));
                         }});
    commands_.push_back({"feature.chamfer", "Chamfer", "Bevel every edge of the selected body",
                         "chamfer", oneWithEdges("feature.chamfer"),
                         [this, declared] {
                             addEdgeFeature("Chamfer", "Chamfer", "distance",
                                            declared("Chamfer", "distance"));
                         }});

    // Hole. Enabled on a single FACE, which is its entire input -- the position and the direction
    // both come from the face, so there is nothing to pick afterwards and nothing to guess.
    //
    // It has computed correctly since the day it was written and no user could reach it, because
    // nothing added it here and both shells build their tools from this catalogue. That is the
    // cheapest kind of gap there is, and Hole is the most-used feature in mechanical CAD.
    commands_.push_back({"feature.hole", "Hole", "Drill a hole into the selected face", "hole",
                         needs("feature.hole"),
                         [this, declared] {
                             addHole(declared("Hole", "diameter"), declared("Hole", "depth"));
                         }});

    // Revolve. Enabled on one EDGE, which identifies the axis and the profile together: the axis is
    // resolved in the profile's own element map, so it must be an edge of the sketch being revolved.
    commands_.push_back({"feature.revolve", "Revolve",
                         "Turn the selected sketch about one of its edges", "revolve",
                         needs("feature.revolve"),
                         [this] { addRevolve(360.0); }});

    // Move. One body, and a vector the user types.
    // Mirror takes a FACE and Pattern takes a BODY, so their enablement differs even though both
    // copy — which the declaration says once and `needs` reads, rather than two predicates here.
    commands_.push_back({"feature.mirror", "Mirror",
                         "Reflect the body about the selected face", "mirror",
                         needs("feature.mirror"), [this] { addMirror(); }});
    commands_.push_back({"feature.pattern", "Pattern",
                         "Repeat the selected body along a direction", "pattern",
                         needs("feature.pattern"),
                         [this, declared] {
                             addPattern(3, declared("Pattern", "dx"), declared("Pattern", "dy"),
                                        declared("Pattern", "dz"));
                         }});

    commands_.push_back({"feature.translate", "Move", "Move the selected body", "move",
                         needs("feature.translate"),
                         [this] { addTranslate(10.0, 0.0, 0.0); }});

    // Measure. A QUERY, not a feature: it changes nothing, so it has no FeatureType and the
    // reachability guard that walks the feature registry cannot see it. It shipped complete, tested
    // and invisible for exactly that reason -- the fifth capability to do so.
    //
    // Enabled whenever there is something to measure, which measureSelection already decides; the
    // command must not have a second opinion about that.
    commands_.push_back({"feature.measure", "Measure",
                         "Measure what is selected: length, area, volume, distance", "measure",
                         [this](const CommandContext&) { return !measureSelection().empty(); },
                         [this] {
                             const auto rows = measureSelection();
                             if (rows.empty()) {
                                 status("Select something to measure.");
                                 return;
                             }
                             // Into the status line, which both shells already show. A dedicated
                             // panel is better and is not a reason to leave the capability
                             // unreachable in the meantime.
                             std::string text;
                             for (const auto& row : rows) {
                                 if (!text.empty()) text += "   ";
                                 text += row.label + " " + row.value;
                             }
                             status(text);
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
