#pragma once

#include "cad/app/Session.h"

#include <QMainWindow>

#include <map>
#include <vector>

class QButtonGroup;
class QDockWidget;
class QLabel;
class QMenu;
class QStackedWidget;
class QTabBar;
class QTableWidget;
class QTreeWidget;

namespace cadqt {

class HomePage;
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
    QTabBar* documentTabs_ = nullptr;

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
