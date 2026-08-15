#pragma once

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
class QTreeWidget;

namespace cadqt {

class HomePage;
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
    void openDemoDocument();

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
    void showOptions();
    void refreshProperties();
    void refreshCommandStates();
    void refreshStatus();
    void refreshDocumentTabs();
    void syncWorkspace();

    void createDocument(cad::app::DocumentKind);

    [[nodiscard]] cad::app::Controller* controller() noexcept { return session_.active(); }

    cad::app::Session session_;

    QTreeWidget* browser_ = nullptr;
    QTableWidget* properties_ = nullptr;
    HomePage* home_ = nullptr;
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
