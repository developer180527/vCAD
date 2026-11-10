#include "cad/app/RibbonLayout.h"

#include "cad/app/Controller.h"

#include <algorithm>
#include <utility>

namespace cad::app {
namespace {

RibbonItem large(std::string id, std::string label, std::string icon, bool panel = false) {
    return RibbonItem{std::move(id), std::move(label), std::move(icon), ItemSize::Large, panel, {}};
}

RibbonItem small(std::string id, std::string label, std::string icon, bool panel = false) {
    return RibbonItem{std::move(id), std::move(label), std::move(icon), ItemSize::Small, panel, {}};
}

/// A command the application will have and does not yet: an EMPTY id, so a shell shows the slot
/// disabled and nothing tries to invoke it.
RibbonItem planned(std::string label, std::string icon, ItemSize size = ItemSize::Small) {
    return RibbonItem{{}, std::move(label), std::move(icon), size, false, {}};
}

RibbonItem tool(std::string id, std::string label, std::string icon, std::string key) {
    RibbonItem item = large(std::move(id), std::move(label), std::move(icon));
    item.checkable = true;
    item.shortcut = std::move(key);
    return item;
}

RibbonItem asking(std::string id, std::string label, std::string icon, std::string what, double initial) {
    RibbonItem item = small(std::move(id), std::move(label), std::move(icon));
    item.valueLabel = std::move(what);
    item.valueDefault = initial;
    return item;
}

}  // namespace

const std::vector<RibbonTab>& partRibbon() {
    // Section ids: the seven the plugin ABI freezes carry its exact strings, because a plugin
    // naming `cad.section.create` must land in THIS section and not in a parallel one. The rest are
    // ours to name, in the same shape, until a plugin needs one of them frozen too.
    static const std::vector<RibbonTab> kTabs{
        RibbonTab{
            "cad.tab.model",
            "3D Model",
            {
                RibbonSection{"cad.section.sketch",
                              "Sketch",
                              {large("feature.sketch", "Start\nSketch", "sketch")}},

                // Create and Modify are Inventor's, command for command and in Inventor's order --
                // two large buttons then small ones stacked three per column, which is why the
                // column breaks fall where they do. Read down each column, not across:
                //
                //   Create   Extrude Revolve | Sweep  Emboss Decal
                //                            | Loft   Derive Import
                //                            | Coil   Rib    Unwrap
                //
                // Almost all of it is disabled, and that is the point: the tab shows what a Part
                // document is FOR, so nothing moves when the commands arrive.
                RibbonSection{"cad.section.create",
                              "Create",
                              {
                                  large("feature.extrude", "Extrude", "extrude", true),
                                  // Revolve takes an angle and defaults to a full turn -- the same
                                  // default computeRevolve uses when none is stored, so the panel
                                  // and the compute agree.
                                  large("feature.revolve", "Revolve", "revolve", true),
                                  planned("Sweep", "sweep"),
                                  planned("Loft", "loft"),
                                  planned("Coil", "coil"),
                                  planned("Emboss", "emboss"),
                                  planned("Derive", "derive"),
                                  planned("Rib", "rib"),
                                  planned("Decal", "decal"),
                                  // A shell action, not a command: it needs a file dialog, and
                                  // `app/` stays toolkit-free so the iPad can use this list too.
                                  small("shell.import", "Import", "import"),
                                  planned("Unwrap", "unwrap"),
                              }},

                // Ours, not Inventor's, and deliberately a SEPARATE panel rather than smuggled into
                // Create. Inventor has no primitive solids in the 3D Model tab: every solid starts
                // as a sketch, so Create begins at Extrude. Keeping primitives in their own panel
                // means Create still matches the reference exactly.
                RibbonSection{"cad.section.primitives",
                              "Primitives",
                              {
                                  large("feature.box", "Box", "box", true),
                                  large("feature.cylinder", "Cylinder", "cylinder", true),
                              }},

                RibbonSection{"cad.section.modify",
                              "Modify",
                              {
                                  // Hole takes a diameter and a depth, so it opens the panel rather
                                  // than drilling a size nobody chose.
                                  large("feature.hole", "Hole", "hole", true),
                                  large("feature.fillet", "Fillet", "fillet"),
                                  small("feature.chamfer", "Chamfer", "chamfer"),
                                  planned("Shell", "shell"),
                                  planned("Draft", "draft"),
                                  planned("Thread", "thread"),
                                  // Inventor's Combine IS the boolean between solid bodies, and
                                  // Join / Cut / Intersect are its three modes -- exactly our Fuse,
                                  // Cut and Common. All three are implemented, so the button gets a
                                  // drop-down rather than silently exposing only one of them.
                                  RibbonItem{"feature.cut",
                                             "Combine",
                                             "combine",
                                             ItemSize::Small,
                                             false,
                                             {"feature.cut", "feature.fuse", "feature.common"}},
                                  planned("Thicken/Offset", "thicken"),
                                  planned("Split", "split"),
                                  // Move sits beside Direct because both edit a body's placement
                                  // rather than its shape. Direct is still a stand-in; Move is real
                                  // and takes a vector.
                                  small("feature.translate", "Move", "move", true),
                                  planned("Direct", "direct"),
                                  planned("Delete Face", "delete-face"),
                                  planned("Mark", "mark"),
                                  planned("Finish", "finish"),
                              }},

                RibbonSection{"cad.section.pattern",
                              "Pattern",
                              {
                                  // Rectangular IS the linear pattern; Circular is still unbuilt and
                                  // stays a placeholder until `rotate` reaches a feature.
                                  small("feature.pattern", "Rectangular", "pattern-rect"),
                                  planned("Circular", "pattern-circular"),
                                  small("feature.mirror", "Mirror", "mirror"),
                              }},

                RibbonSection{"cad.section.edit",
                              "Edit",
                              {
                                  small("edit.undo", "Undo", "undo"),
                                  small("edit.redo", "Redo", "redo"),
                                  small("edit.delete", "Delete", "delete"),
                              }},

                // Rollback gets its own section rather than sitting in Edit: it is not an edit at
                // all -- it moves where you are in the tree, and grouping it with Undo and Delete
                // would suggest otherwise.
                RibbonSection{"cad.section.history",
                              "History",
                              {
                                  small("edit.rollback", "Roll Back", "rollback"),
                                  small("edit.rollforward", "Roll Forward", "rollforward"),
                              }},
            }},

        RibbonTab{"cad.tab.sketch",
                  "Sketch",
                  {RibbonSection{"cad.section.sketch.manage",
                                 "Manage",
                                 {
                                     large("feature.sketch", "Start\nSketch", "sketch"),
                                     large("sketch.edit", "Edit\nSketch", "sketch-edit"),
                                     planned("Delete\nSketch", "delete", ItemSize::Large),
                                 }}}},

        RibbonTab{"cad.tab.inspect",
                  "Inspect",
                  {
                      RibbonSection{"cad.section.measure",
                                    "Measure",
                                    {large("feature.measure", "Measure", "measure")}},
                      RibbonSection{"cad.section.analysis",
                                    "Analysis",
                                    {
                                        planned("Section\nView", "section", ItemSize::Large),
                                        planned("Mass Properties", "mass"),
                                        planned("Draft Analysis", "draft"),
                                    }},
                  }},

        RibbonTab{"cad.tab.annotate",
                  "Annotate",
                  {RibbonSection{"cad.section.annotation",
                                 "3D Annotation",
                                 {
                                     planned("Dimension", "dimension", ItemSize::Large),
                                     planned("Note", "note", ItemSize::Large),
                                 }}}},

        RibbonTab{"cad.tab.manage",
                  "Manage",
                  {
                      // Parameters is real, and on the Manage tab where Inventor and Fusion both
                      // put it. A shell action because it opens a dialog.
                      RibbonSection{"cad.section.parameters",
                                    "Parameters",
                                    {large("shell.parameters", "Parameters", "parameters")}},
                      // The DDC, surfaced in the UI. No other CAD application has this panel
                      // because no other CAD application has a content-addressed recompute cache
                      // (ADR 0004).
                      RibbonSection{"cad.section.cache",
                                    "Cache",
                                    {
                                        planned("Cache\nStatus", "cache", ItemSize::Large),
                                        planned("Purge Local", "purge"),
                                    }},
                  }},

        RibbonTab{"cad.tab.view",
                  "View",
                  {
                      RibbonSection{"cad.section.navigate",
                                    "Navigate",
                                    {
                                        // A VISIBLE way to rotate the view. Orbit is otherwise on
                                        // the middle button or on Alt, and neither is discoverable:
                                        // a laptop has no middle button, and nobody finds a modifier
                                        // chord by looking at the screen. A shell action because it
                                        // is a checkable MODE, not a command.
                                        large("shell.orbit", "Orbit", "ortho"),
                                        large("view.fit", "Fit", "fit"),
                                        large("view.ortho", "Ortho", "ortho"),
                                    }},
                      RibbonSection{"cad.section.appearance",
                                    "Appearance",
                                    {
                                        planned("Shaded", "shaded"),
                                        planned("Shaded + Edges", "shaded-edges"),
                                        planned("Wireframe", "wireframe"),
                                    }},
                      RibbonSection{"cad.section.visibility",
                                    "Visibility",
                                    {
                                        planned("Origin Planes", "origin"),
                                        planned("All Sketches", "sketches"),
                                    }},
                  }},
    };
    return kTabs;
}

const std::vector<RibbonTab>& sketchRibbon() {
    // Transcribed from shell_qt's MainWindow (the sketch branch of rebuildRibbon), item for item and
    // in its order, so that moving it is a refactor and not a redesign.
    static const std::vector<RibbonTab> kTabs{RibbonTab{
        "cad.tab.sketch.edit",
        "Sketch",
        {
            // The tool is set on the CONTROLLER, not on a canvas: it must mean the same thing to
            // every shell. Letters as Inventor and SolidWorks bind them; R for rectangle, T for
            // trim (used in the same breath as drawing), D for dimension.
            RibbonSection{"cad.section.sketch.draw",
                          "Draw",
                          {
                              tool("sketch.tool.select", "Select", "select", "S"),
                              tool("sketch.tool.line", "Line", "line", "L"),
                              tool("sketch.tool.circle", "Circle", "circle", "C"),
                              tool("sketch.tool.rectangle", "Rectangle", "rectangle", "R"),
                              tool("sketch.tool.trim", "Trim", "trim", "T"),
                              tool("sketch.tool.dimension", "Dimension", "dimension", "D"),
                          }},
            RibbonSection{"cad.section.sketch.view",
                          "View",
                          {
                              // Back to facing the plane after orbiting away to see the part.
                              [] {
                                  RibbonItem item = large("sketch.lookat", "Look At", "ortho");
                                  item.shortcut = "F";
                                  return item;
                              }(),
                              // A mode you stay in while drawing on a buried face.
                              [] {
                                  RibbonItem item = large("sketch.slice", "Slice", "section");
                                  item.checkable = true;
                                  return item;
                              }(),
                          }},
            // Enabled from the selection (Controller::applicableConstraints), so a button is live
            // only when the model would accept it.
            RibbonSection{"cad.section.sketch.constrain",
                          "Constrain",
                          {
                              small("sketch.constrain.horizontal", "Horizontal", "horizontal"),
                              small("sketch.constrain.vertical", "Vertical", "vertical"),
                              small("sketch.constrain.parallel", "Parallel", "parallel"),
                              small("sketch.constrain.perpendicular", "Perpendicular", "perpendicular"),
                              small("sketch.constrain.equal", "Equal", "equal"),
                              small("sketch.constrain.tangent", "Tangent", "tangent"),
                              asking("sketch.constrain.radius", "Radius", "radius", "Radius", 10.0),
                          }},
            RibbonSection{"cad.section.sketch.modify",
                          "Modify",
                          {
                              // Negative offsets to the other side: the inner wall, not the outer.
                              asking("sketch.offset", "Offset", "offset", "Distance", 5.0),
                              small("sketch.delete", "Delete", "delete"),
                          }},
            RibbonSection{"cad.section.sketch.exit",
                          "Exit",
                          {
                              large("sketch.finish", "Finish\nSketch", "sketch-finish"),
                              small("sketch.cancel", "Cancel", "delete"),
                          }},
        }}};
    return kTabs;
}

const std::vector<RibbonTab>& ribbonFor(const Controller& controller) {
    return controller.environment() == Environment::Sketch ? sketchRibbon() : partRibbon();
}

namespace {

using Tool = Controller::SketchTool;
using Kind = sketch::ConstraintKind;

const std::pair<const char*, Tool> kTools[] = {
    {"sketch.tool.select", Tool::Select}, {"sketch.tool.line", Tool::Line},   {"sketch.tool.circle", Tool::Circle},
    {"sketch.tool.rectangle", Tool::Rectangle}, {"sketch.tool.trim", Tool::Trim}, {"sketch.tool.dimension", Tool::Dimension},
};

const std::pair<const char*, Kind> kConstraints[] = {
    {"sketch.constrain.horizontal", Kind::Horizontal},     {"sketch.constrain.vertical", Kind::Vertical},
    {"sketch.constrain.parallel", Kind::Parallel},         {"sketch.constrain.perpendicular", Kind::Perpendicular},
    {"sketch.constrain.equal", Kind::EqualLength},         {"sketch.constrain.tangent", Kind::Tangent},
    {"sketch.constrain.radius", Kind::Radius},
};

const Tool* toolFor(const std::string& id) {
    for (const auto& [name, t] : kTools)
        if (id == name) return &t;
    return nullptr;
}

const Kind* constraintFor(const std::string& id) {
    for (const auto& [name, k] : kConstraints)
        if (id == name) return &k;
    return nullptr;
}

const Command* commandFor(const Controller& c, const std::string& id) {
    for (const auto& command : c.commands())
        if (command.id == id) return &command;
    return nullptr;
}

}  // namespace

RibbonItemState ribbonItemState(const Controller& c, const std::string& id) {
    if (id.empty() || id.rfind("shell.", 0) == 0) return {};
    const bool sketching = c.environment() == Environment::Sketch;

    // Checked from the CONTROLLER, not from which button was pressed last: finishing a sketch
    // resets the tool, and a toolbar still showing Line lit is how a user stops trusting it.
    if (const Tool* t = toolFor(id)) return {sketching, sketching && c.sketchTool() == *t};
    if (const Kind* k = constraintFor(id)) {
        if (!sketching) return {};
        const auto applicable = c.applicableConstraints();
        return {std::find(applicable.begin(), applicable.end(), *k) != applicable.end(), false};
    }
    if (id == "sketch.lookat") return {sketching, false};
    if (id == "sketch.slice") return {sketching, c.sliceEnabled()};
    if (id == "sketch.offset" || id == "sketch.delete") return {sketching, false};

    if (const Command* command = commandFor(c, id))
        return {!command->enabled || command->enabled(c.context()), false};
    return {};
}

bool runRibbonItem(Controller& c, const std::string& id, std::optional<double> value) {
    if (!ribbonItemState(c, id).enabled) return false;
    if (const Tool* t = toolFor(id)) {
        c.setSketchTool(*t);
        return true;
    }
    if (id == "sketch.constrain.radius") return value && c.applySketchRadius(*value);
    if (const Kind* k = constraintFor(id)) return c.applySketchConstraint(*k);
    if (id == "sketch.lookat") {
        c.alignCameraToSketch();
        return true;
    }
    if (id == "sketch.slice") {
        c.setSliceEnabled(!c.sliceEnabled());
        return true;
    }
    if (id == "sketch.offset") return value && c.offsetSketchSelection(*value);
    if (id == "sketch.delete") {
        c.deleteSketchSelection();
        return true;
    }
    if (const Command* command = commandFor(c, id)) {
        command->invoke();
        return true;
    }
    return false;
}

}  // namespace cad::app
