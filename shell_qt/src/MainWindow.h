#pragma once

// For CadSession: the plugin host session is a member, so the handle type must be complete here.
#include "cad/abi/cad_plugin_abi.h"

#include "CadHomeModel.h"

#include <QPixmap>
#include "cad/app/Session.h"
#include "proshell/ShellWindow.h"

#include <map>
#include <vector>

class QButtonGroup;
class QDockWidget;
class QFormLayout;
class QLabel;
class QMenu;
class QSplitter;
class QStackedWidget;
class QTabBar;
class QTableWidget;
class QToolButton;
class QTreeWidget;

namespace proshell {
class HomePage;
}

// At global scope, NOT inside `namespace cadqt`. Declared inside, it becomes `cadqt::proshell` and
// shadows the real namespace for the whole header — every existing `proshell::ShellWindow` then
// resolves to a type that does not exist. The same mistake this shell made once with a
// `class QFormLayout*` inside a namespace.
namespace proshell { class Settings; }

namespace cadqt {

class PluginManager;
class SketchCanvas;
class Viewport;


/// vCAD, on proshell's frame.
///
/// The frame -- quick access strip, ribbon, docks, workspace stack, document tabs, status bar --
/// is `proshell::ShellWindow` and knows nothing about CAD. What is left here is the part that IS
/// CAD: the command catalogue, the feature browser, the property table, the sketch canvases, and
/// the session that owns the documents.
///
/// Documents are deliberately still ours. ShellWindow hands over the tab bar and the page stack
/// rather than an abstract document model, because every idea in `cad::app::Session` -- an
/// environment, a controller per document, "Home is not a document" -- is vCAD's idea about
/// documents rather than a fact about applications. See the note on ShellWindow.
class MainWindow : public proshell::ShellWindow {
    Q_OBJECT
public:
    MainWindow();
    ~MainWindow() override;

    /// Screenshot support (`--shot`). Selecting a ribbon tab and opening a populated document
    /// are the two things a UI screenshot needs and a fresh window does not do on its own.
    void selectRibbonTab(int index);
    /// Applies the stored theme. Called from main() at startup, so the window is painted in the
    /// user's scheme rather than flashing the default first.
    void restoreTheme();

    void openDemoDocument();
    /// Screenshot support for the plugin manager, which is a window of its own.
    void openPluginManagerForShot();

    /// Enters a sketch and draws two lines, for `--shot --sketch`.
    ///
    /// Here because "the sketch is drawn in the 3D viewport" is a claim about PIXELS. It was
    /// previously true that sketching swapped to a different widget entirely, and no test noticed
    /// — the only thing that can tell the difference is a picture.
    void drawSketchForShot(int lines = 2);

    /// Selects a row of the model browser, for `--shot --select N`.
    ///
    /// Through the browser rather than through Controller::select, so what the screenshot proves is
    /// the whole path a user takes: click a row, the controller marks the geometry, the viewport
    /// repaints. Calling the controller directly would skip exactly the part that was broken.
    void selectBrowserRowForShot(int row);
    [[nodiscard]] QPixmap grabPluginManager();

    /// Renders the settings window for `--shot --settings [pageId]`.
    ///
    /// Here for the same reason the ribbon has a shot mode: a settings page contributed by a plugin
    /// is a claim about pixels, and one that is only ever described is one nobody has checked.
    [[nodiscard]] QPixmap grabSettingsForShot(const QString& pageId = {});

protected:
    /// Prompts for unsaved work in EVERY open document before letting the window go.
    [[nodiscard]] bool confirmClose() override;

private:
    void buildQuickAccess();      ///< vCAD's buttons and File menu on the frame's strip
    void buildBrowserAndProperties();
    void buildWorkspaces();
    void buildStatusFields();

    /// Rebuilds the ribbon's tabs for the active workspace.
    ///
    /// Ribbon tabs are DERIVED from what you are editing, not registered globally (ADR 0009).
    /// That is the anti-workbench decision made concrete: a user never selects a command set,
    /// they select a thing to edit and the commands follow.
    void rebuildRibbon();
    /// Import lives here rather than in Controller's command registry because it needs a file
    /// dialog, and app/ carries no toolkit. See the comment at the call site.
    void importFile();

    // ── files ────────────────────────────────────────────────────────────────────────
    void openDocument();
    void openPath(const QString&);
    /// `saveAs` forces the dialog; otherwise it appears only when the document has no path.
    /// Returns false if the save did not happen, which callers must respect — see
    /// confirmDiscardChanges.
    bool saveDocument(bool saveAs);
    /// Save / Discard / Cancel prompt. False means the user cancelled and the caller must abort.
    bool confirmDiscardChanges();
    void syncTitle();
    /// Greys the Constrain buttons that do not suit the current sketch selection.
    void refreshSketchConstraintStates();
    /// The left dock's command panel: OK/Cancel plus a field per parameter.
    [[nodiscard]] QWidget* buildCommandPanel();
    /// Swaps the left dock between the tree and the running command, rebuilding its fields.
    void syncCommandPanel();
    /// A ribbon action that opens the command panel when the command has parameters, and invokes
    /// directly when it does not.
    [[nodiscard]] QAction* parameterised(const char* id, const QString& label,
                                         const QString& iconName);

    /// The action for a real command from `Controller::commands()`, or null if it has none.
    [[nodiscard]] QAction* command(const char* id);
    /// A disabled stand-in for a command the app does not have yet. See the definition.
    [[nodiscard]] QAction* planned(const QString& label, const QString& iconName);
    /// The real command if `Controller` exposes it, otherwise a disabled stand-in. Lets the
    /// ribbon be written once against the full command set: a command becomes live the day
    /// `app/` grows it, with no edit here.
    [[nodiscard]] QAction* commandOr(const char* id, const QString& label,
                                     const QString& iconName);

    void refreshTree();
    void showBrowserMenu(const QPoint&);
    void showPluginManager();
    void showOptions();

    /// Declares vCAD's settings into the shell's store, once.
    ///
    /// The store is `proshell`'s and knows nothing about CAD; this is where the ids, labels and
    /// defaults that ARE about CAD get named. A second application built on the same shell declares
    /// its own and shares none of them.
    /// Lights the active drawing tool and greys them all outside the sketch environment.
    void refreshSketchToolStates();

    void declareSettings();

    /// Loads installed plugins, once, before any UI reads what they contribute.
    ///
    /// Failures are reported and never fatal: a stale plugin after an update must leave the
    /// application usable, which is the whole reason the loader counts failures rather than
    /// returning on the first one.
    void loadPlugins();

    /// Adds pages plugins declared to the settings store. Called from declareSettings, after the
    /// built-in pages, so a plugin cannot displace a built-in setting.
    void addPluginSettings();


    /// Pushes a changed setting into the running controllers.
    ///
    /// Applied to EVERY open document, not just the active one: units are a property of the USER,
    /// not of a file, and the tab you switch to still showing millimetres would read as a bug.
    void applySetting(const QString& id);
    void refreshProperties();
    void refreshCommandStates();
    void refreshStatus();
    void refreshDocumentTabs();
    void syncWorkspace();

    void createDocument(cad::app::DocumentKind);

    [[nodiscard]] cad::app::Controller* controller() noexcept { return session_.active(); }

    cad::app::Session session_;
    /// The session plugins are loaded into, held for the life of the window.
    ///
    /// Separate from any document's session and never released early: a plugin's descriptors point
    /// into memory this session owns, so releasing it while the settings window is open would leave
    /// that window rendering freed strings.
    CadSession pluginSession_ = 0;

    /// Non-empty when a plugin failed to load, shown once the status bar exists.
    QString pluginLoadWarning_;

    struct SketchToolAction {
        QAction* action = nullptr;
        cad::app::Controller::SketchTool tool{};
    };
    std::vector<SketchToolAction> sketchToolActions_;

    /// The Orbit toggle in the View tab, kept so its checked state can follow the controller.
    QAction* orbitAction_ = nullptr;
    /// The same mode on the always-visible strip, since the View tab is hidden while sketching.
    QToolButton* orbitButton_ = nullptr;

    /// Sets orbit mode and brings both controls into line with it.
    void setOrbitMode(bool on);

    proshell::Settings* settings_ = nullptr;
    /// Answers proshell's home-page questions from the session. Declared after it, since it
    /// holds a reference.
    CadHomeModel homeModel_{session_};

    PluginManager* pluginManager_ = nullptr;
    QTreeWidget* browser_ = nullptr;
    QTableWidget* properties_ = nullptr;
    proshell::HomePage* home_ = nullptr;
    /// One sketch canvas per open document, parallel to editors_. Held rather than looked up so
    /// switching documents while sketching cannot show another document's sketch.
    std::vector<SketchCanvas*> sketchCanvases_;
    /// Constrain buttons plus what each needs selected. Rebuilt with the ribbon, because the
    /// actions are owned by the tab that is destroyed on every environment change.
    struct SketchConstraintAction {
        QAction* action = nullptr;
        std::size_t needs = 1;
        bool linesOnly = true;
    };
    std::vector<SketchConstraintAction> sketchConstraintActions_;

    QWidget* commandPanel_ = nullptr;
    QLabel* commandTitle_ = nullptr;
    QFormLayout* commandFields_ = nullptr;
    /// Panel-opening actions paired with the real command, so enablement stays in step.
    struct ParameterisedAction {
        QAction* action = nullptr;
        QAction* real = nullptr;
    };
    std::vector<ParameterisedAction> parameterisedActions_;
    /// Last seen environment, so a change can be detected from a document notification. See the
    /// comment at the observer.
    cad::app::Environment lastEnvironment_ = cad::app::Environment::Model;

    /// One editor widget per open document, parallel to Session::documents(). Held rather than
    /// recreated so switching tabs preserves camera and scroll position.
    std::vector<QWidget*> editors_;

    /// Body / Face / Edge / Vertex. Shell-owned until IPicker can resolve by entity type.
    QWidget* filterBar_ = nullptr;
    QButtonGroup* selectionFilter_ = nullptr;

    QLabel* statusStats_ = nullptr;
    QLabel* statusUnits_ = nullptr;

    std::map<std::string, QAction*> actions_;
    bool updatingUi_ = false;
};

}  // namespace cadqt
