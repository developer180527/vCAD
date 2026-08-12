#pragma once

#include "cad/app/Session.h"

#include <QMainWindow>

#include <map>
#include <vector>

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
    QTreeWidget* browser_ = nullptr;
    QTableWidget* properties_ = nullptr;
    QStackedWidget* workspaces_ = nullptr;
    HomePage* home_ = nullptr;
    QTabBar* documentTabs_ = nullptr;

    /// One editor widget per open document, parallel to Session::documents(). Held rather than
    /// recreated so switching tabs preserves camera and scroll position.
    std::vector<QWidget*> editors_;

    QLabel* statusMessage_ = nullptr;
    QLabel* statusStats_ = nullptr;
    QLabel* statusUnits_ = nullptr;

    std::map<std::string, QAction*> actions_;
    bool updatingUi_ = false;
};

}  // namespace cadqt
