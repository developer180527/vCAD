#pragma once

#include "cad/app/Controller.h"

#include <QMainWindow>
#include <QTreeWidget>

#include <map>
#include <memory>

class QLabel;
class QTableWidget;

namespace cadqt {

class Ribbon;
class ViewportPlaceholder;

/// Inventor's layout: ribbon on top, model browser left, viewport centre, properties right,
/// status bar bottom.
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow();
    ~MainWindow() override;

private:
    void buildRibbon();
    void buildDocks();
    void buildStatusBar();

    /// Rebuilds the browser from `Controller::tree()`.
    ///
    /// Full rebuild rather than an incremental diff, and that is a considered choice: a feature
    /// tree is tens to low thousands of rows, a rebuild is microseconds, and incremental tree
    /// updates are a classic source of "the tree disagrees with the document" bugs. Revisit when
    /// a profile says to, not before.
    void refreshTree();
    void refreshProperties();
    void refreshCommandStates();
    void refreshStatus();

    cad::app::Controller controller_;

    Ribbon* ribbon_ = nullptr;
    QTreeWidget* browser_ = nullptr;
    QTableWidget* properties_ = nullptr;
    ViewportPlaceholder* viewport_ = nullptr;

    QLabel* statusMessage_ = nullptr;
    QLabel* statusStats_ = nullptr;
    QLabel* statusUnits_ = nullptr;

    /// Command id -> action, so the ribbon and the menus share one action per command and
    /// enablement is applied in one place.
    std::map<std::string, QAction*> actions_;

    /// Guards the tree's own selection signal while we are writing into it, so a programmatic
    /// refresh cannot look like a user click and recurse.
    bool updatingTree_ = false;
};

}  // namespace cadqt
