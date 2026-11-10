#pragma once

/// Where every command sits in the ribbon, as DATA rather than as code in a shell.
///
/// # Why this is not in the shell
///
/// The arrangement was 97 `addPanel`/`addLarge`/`addSmall` calls in `shell_qt/src/MainWindow.cpp`,
/// and the iPad shell hardcoded its own rail beside it: 25 command ids named in one, 20 in the
/// other. Two shells hand-building one arrangement drift, which is why
/// `tests/acceptance/feature_reachability.cpp` had to exist at all -- and that guard reads the
/// shells' SOURCE TEXT, so it passes or fails on how the code is spelled rather than on what the
/// application offers.
///
/// One list, consumed by every shell, makes the drift impossible instead of detectable.
///
/// # Why it lives in `app/` and not in `proshell`
///
/// `proshell` is the domain-free shell module: a ribbon, a theme, a marking menu, and no knowledge
/// of what a solid is. "Extrude goes in Create, next to Revolve" is domain knowledge, so it belongs
/// with the application. What `proshell` gets is the widgets; what it must never get is this file.
///
/// # The ids are the plugin ABI's
///
/// Tab and section ids are the same strings `cad_plugin_abi.h` freezes -- `cad.tab.model`,
/// `cad.section.create`. A plugin registering a command names the section it wants, and it must
/// land beside the built-ins rather than in a parallel structure the shell assembles separately.
/// One vocabulary for both is the whole point; see ADR 0011's note on what a plugin may add.
///
/// # What a placeholder is doing in the application layer
///
/// `MainWindow::planned()` used to say, correctly for its time, that a disabled label is a shell
/// concern and `app/` exposes only commands that exist. That holds for `Controller::commands()`,
/// which still lists only the real ones. But the ARRANGEMENT is shared between shells, including
/// which slots are declared-but-unbuilt -- an iPad that quietly omitted Loft would diverge from the
/// desktop and nothing would say so. So the slot lives here and the RENDERING of it stays the
/// shell's: disabled, with its own "not implemented yet" wording.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace cad::app {

class Controller;

/// Icon above label, or icon beside it. Inventor's two button sizes, and the reason a panel's
/// small buttons stack three to a column.
enum class ItemSize : std::uint8_t { Large, Small };

/// One button.
struct RibbonItem {
    /// A command id from `Controller::commands()`, a `shell.` id the shell binds itself, or EMPTY
    /// for a planned command that does not exist yet.
    ///
    /// The `shell.` prefix is for actions that cannot be commands: Import needs a file dialog and
    /// Parameters needs a dialog, and `app/` stays toolkit-free so the iPad shell can use it. The
    /// slot is still declared here, because a shell silently missing one is the divergence this
    /// file exists to prevent.
    std::string id;

    /// Shown to the user. May contain a newline, which is how "Start\nSketch" gets two lines under
    /// a 32 px icon without the label eliding.
    std::string label;

    /// Resolved by the shell's own theme. A name rather than an image: the desktop paints these
    /// programmatically and a Rust or SwiftUI shell would draw its own, from the same name.
    std::string iconName;

    ItemSize size = ItemSize::Small;

    /// Whether clicking opens the command's parameter panel rather than invoking it directly.
    ///
    /// Carried rather than derived, and deliberately so FOR NOW. It could be computed -- a feature
    /// whose declaration has values wants a panel -- but the current set was hand-picked, and
    /// deriving it would change behaviour: Pattern declares four values and today invokes directly
    /// with its defaults. Recording the choice keeps this change a refactor, and makes the
    /// inconsistency visible to whoever decides to settle it.
    bool opensPanel = false;

    /// Modes offered on a drop-down, as command ids. Empty for an ordinary button.
    ///
    /// Inventor's Combine is one button whose menu is Join, Cut and Intersect -- three commands, one
    /// slot. A shell that cannot draw a menu may show the first and ignore the rest.
    std::vector<std::string> menu;

    /// A mode the button stays lit for (a sketch tool, Slice), rather than an action that fires and
    /// is done. Whether it is lit NOW is ribbonItemState()'s answer, never the shell's memory of
    /// which button was pressed last.
    bool checkable = false;

    /// The key that does the same thing, as it should be shown: "L". Empty for none.
    std::string shortcut;

    /// For an item that needs one number before it can run (Offset, Radius): what to ask for, and
    /// the starting value in millimetres. Empty label means it runs directly. A shell asks however
    /// it asks -- a field in its Properties panel, a dialog -- and passes the answer to
    /// runRibbonItem.
    std::string valueLabel;
    double valueDefault = 0.0;
};

struct RibbonSection {
    std::string id;      ///< `cad.section.create`, frozen in the plugin ABI
    std::string label;   ///< shown; may be translated freely
    std::vector<RibbonItem> items;
};

struct RibbonTab {
    std::string id;      ///< `cad.tab.model`, frozen in the plugin ABI
    std::string label;
    std::vector<RibbonSection> sections;
};

/// vCAD's Part environment: the tabs a part document shows.
///
/// Inventor's layout, command for command and in Inventor's order -- we are not designing a ribbon,
/// we are copying one people already know.
[[nodiscard]] const std::vector<RibbonTab>& partRibbon();

/// The sketch environment: the ONE tab the ribbon collapses to while a sketch is open, as
/// Inventor's does. Its ids are `sketch.` actions that are not Controller commands -- choosing a
/// tool, applying a constraint -- so they are resolved by runRibbonItem below rather than by
/// Controller::commands(), except Finish and Cancel, which are commands.
[[nodiscard]] const std::vector<RibbonTab>& sketchRibbon();

/// The tabs for whatever environment the controller is in.
[[nodiscard]] const std::vector<RibbonTab>& ribbonFor(const Controller&);

/// Whether a ribbon item can be pressed and whether it is lit, from the MODEL: for a command its
/// `enabled` predicate, for a sketch tool whether it is the active one, for a constraint whether
/// the selection could take it. `shell.` ids are the shell's own and answer {false, false}.
struct RibbonItemState {
    bool enabled = false;
    bool checked = false;
};
[[nodiscard]] RibbonItemState ribbonItemState(const Controller&, const std::string& id);

/// Runs an item by id: a Controller command, or a `sketch.` action. `value` is the answer to the
/// item's valueLabel, in millimetres, for the items that ask. Returns false if the id is unknown,
/// disabled, or needed a value it was not given. `shell.` ids are never run here.
bool runRibbonItem(Controller&, const std::string& id, std::optional<double> value = std::nullopt);

}  // namespace cad::app
