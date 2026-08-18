#pragma once

/// The value types a shell reads from and writes to `Controller`.
///
/// Split out of Controller.h, which had grown to 974 lines. These are DATA — a tree row, a command,
/// a preference — and none of them mentions the Controller. A shell building a model tree or a
/// ribbon needs this file and can ignore the class entirely, which was impossible while the two
/// were one header.
///
/// The same argument as proshell's SettingsModel: describing the surface as data rather than as
/// methods is what lets a second front end render it without inheriting anything.

#include "cad/document/Document.h"
#include "cad/units/Units.h"
#include "cad/render/Camera.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace cad::app {

enum class TreeGroup : std::uint8_t {
    History,    ///< features, in the order they were applied
    Origin,     ///< datum planes, and later axes and the origin point
};

struct TreeItem {
    document::ObjectId id;
    std::string label;
    std::string type;
    document::ObjectState state = document::ObjectState::Dirty;
    std::string error;              ///< empty unless state is Failed or Blocked
    bool selected = false;
    bool visible = true;
    TreeGroup group = TreeGroup::History;
};

/// What a command needs to know before offering itself. A ribbon button that is enabled and then
/// fails is worse than one that is greyed out.
struct CommandContext {
    std::size_t selectedObjects = 0;
    std::size_t selectedElements = 0;
    bool documentEmpty = true;
    bool canUndo = false;
    bool canRedo = false;
};

/// A user-invocable action. The shell renders these; it does not know what they do.
struct Command {
    std::string id;                 ///< "feature.box", stable, used by shortcuts and tests
    std::string label;
    std::string tooltip;
    std::string iconName;           ///< resolved by the shell's theme
    std::function<bool(const CommandContext&)> enabled;
    std::function<void()> invoke;
};

/// What the app is currently doing, which changes what a click MEANS.
///
/// The concept DESKTOP_UX 3.1 identified as missing and three future features need: the sketch
/// editor, assembly edit-in-place, and drawing sheets. It lives here rather than in the shell
/// because the rule "in a sketch, clicks hit sketch geometry" is a model rule, not a Qt one -- put
/// it in MainWindow and it does not exist on iPad.
enum class Environment : std::uint8_t {
    Model,   ///< the default: features, solids, the browser
    Sketch,  ///< editing one sketch's geometry and constraints
};

const char* toString(Environment) noexcept;

/// One editable input of a command in progress.
///
/// Text in and text out, exactly like PropertyRow: the shell renders a field, the user types, and
/// the parsing and unit handling stay here rather than in every shell. `kind` is a rendering hint
/// only — a Bool wants a checkbox, a Length wants a line edit that accepts "2 in".
struct CommandParameter {
    enum class Kind : std::uint8_t { Length, Angle, Real, Integer, Text, Bool };
    std::string name;      ///< stable key the command reads
    std::string label;     ///< shown to the user
    Kind kind = Kind::Length;
    std::string value;     ///< current text, already formatted with units
};

/// User preferences that change how the application behaves, not what the model is.
///
/// In `app/` rather than the shell because every one of these is consumed BELOW the shell: display
/// units decide how Controller formats a property, the navigation preset is read by the camera, and
/// the sketch tolerances are passed to inference. A shell that owned them would have to push each
/// one down on every change, and the iPad shell would have to do it again.
///
/// Deliberately NOT part of the document. A colleague opening your file should get their own units
/// and their own mouse, not yours.
struct Preferences {
    /// How lengths are SHOWN and how a bare number is READ. Storage is always millimetres; this
    /// never changes what is in the file.
    units::UnitSystem displayUnits = units::UnitSystem::Millimetre;

    /// Which mouse button orbits. The single most personal setting in any CAD application, and the
    /// first thing someone changes when the app does not match the one they came from.
    render::NavigationPreset navigation = render::NavigationPreset::Cad;

    /// Sketch constraint inference, in document units and degrees. Exposed because the right value
    /// depends on the drawings a user actually receives — see Infer.h on why guessing is unsafe.
    double snapTolerance = 0.01;
    double angleTolerance = 0.5;
};

}  // namespace cad::app
