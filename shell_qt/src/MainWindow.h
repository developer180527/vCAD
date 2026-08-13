#pragma once

#include "cad/app/Session.h"

#include <QMainWindow>

#include <map>
#include <vector>

class QButtonGroup;
class QDockWidget;
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
class Ribbon;
class ViewportPlaceholder;

/// Inventor's frame: quick access toolbar, ribbon, docks, stacked workspaces, document tabs at
/// the bottom, status bar.
class MainWindow : public QMainWindow {
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
    void closeEvent(QCloseEvent*) override;

private:
    void buildTopArea();          ///< QAT + ribbon, in ONE menu widget
    void buildDocks();
    void buildWorkspaces();
    void buildStatusBar();

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
    void refreshProperties();
    void refreshCommandStates();
    void refreshStatus();
    void refreshDocumentTabs();
    void syncWorkspace();

    void createDocument(cad::app::DocumentKind);

    [[nodiscard]] cad::app::Controller* controller() noexcept { return session_.active(); }

    cad::app::Session session_;

    Ribbon* ribbon_ = nullptr;
    QMenu* fileMenu_ = nullptr;
    /// Held so Home can hide them: Home is a full-window workspace, not a document.
    QDockWidget* browserDock_ = nullptr;
    QDockWidget* propertiesDock_ = nullptr;

    QTreeWidget* browser_ = nullptr;
    QTableWidget* properties_ = nullptr;
    QStackedWidget* workspaces_ = nullptr;
    HomePage* home_ = nullptr;
    /// Home's left rail. Built by HomePage but laid out here, so it spans the full window height
    /// past the document tab bar rather than being clipped by it.
    QWidget* homeSidebar_ = nullptr;
    /// Carries the drag handle between the rail and the content column.
    QSplitter* homeSplitter_ = nullptr;
    QTabBar* documentTabs_ = nullptr;
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
    /// Last seen environment, so a change can be detected from a document notification. See the
    /// comment at the observer.
    cad::app::Environment lastEnvironment_ = cad::app::Environment::Model;

    /// One editor widget per open document, parallel to Session::documents(). Held rather than
    /// recreated so switching tabs preserves camera and scroll position.
    std::vector<QWidget*> editors_;

    /// Body / Face / Edge / Vertex. Shell-owned until IPicker can resolve by entity type.
    QWidget* filterBar_ = nullptr;
    QButtonGroup* selectionFilter_ = nullptr;

    QLabel* statusMessage_ = nullptr;
    QLabel* statusStats_ = nullptr;
    QLabel* statusUnits_ = nullptr;

    std::map<std::string, QAction*> actions_;
    bool updatingUi_ = false;
};

}  // namespace cadqt
