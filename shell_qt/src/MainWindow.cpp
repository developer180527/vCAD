#include "MainWindow.h"

#include "Icons.h"
#include "Ribbon.h"
#include "ViewportPlaceholder.h"

#include <QDockWidget>
#include <QHeaderView>
#include <QLabel>
#include <QMenuBar>
#include <QStatusBar>
#include <QTableWidget>
#include <QVBoxLayout>

namespace cadqt {
namespace {

/// Per-state colour for the browser. A failed feature must be obvious at a glance — FreeCAD marks
/// errors subtly enough that people miss them and then wonder why the model is wrong.
QColor stateColour(cad::document::ObjectState state) {
    switch (state) {
        case cad::document::ObjectState::Clean:   return QColor(0xdc, 0xdf, 0xe4);
        case cad::document::ObjectState::Dirty:   return QColor(0xd8, 0xa5, 0x4a);
        case cad::document::ObjectState::Failed:  return QColor(0xe0, 0x62, 0x5c);
        case cad::document::ObjectState::Blocked: return QColor(0x9a, 0x7a, 0x50);
    }
    return QColor(0xdc, 0xdf, 0xe4);
}

}  // namespace

MainWindow::MainWindow() {
    setWindowTitle(tr("vCAD — Untitled"));
    resize(1440, 900);

    buildRibbon();
    buildDocks();
    buildStatusBar();

    viewport_ = new ViewportPlaceholder(controller_, this);
    setCentralWidget(viewport_);

    controller_.onDocumentChanged([this] {
        refreshTree();
        refreshProperties();
        refreshCommandStates();
        refreshStatus();
        if (viewport_ != nullptr) viewport_->update();
    });
    controller_.onViewChanged([this] {
        if (viewport_ != nullptr) viewport_->update();
        refreshStatus();
    });
    controller_.onStatus([this](const std::string& text) {
        statusMessage_->setText(QString::fromStdString(text));
    });

    refreshCommandStates();
    refreshStatus();
}

MainWindow::~MainWindow() = default;

void MainWindow::buildRibbon() {
    ribbon_ = new Ribbon(this);

    // One QAction per command, shared by the ribbon and the menu bar, so enablement is decided
    // once in refreshCommandStates rather than in each place a command appears.
    for (const auto& command : controller_.commands()) {
        auto* action = new QAction(QString::fromStdString(command.label), this);
        action->setToolTip(QString::fromStdString(command.tooltip));
        action->setIcon(icon(QString::fromStdString(command.iconName)));
        const auto invoke = command.invoke;
        connect(action, &QAction::triggered, this, [invoke] { invoke(); });
        actions_[command.id] = action;
    }

    // Standard shortcuts, which users expect to work regardless of the ribbon.
    if (auto* undo = actions_["edit.undo"]) undo->setShortcut(QKeySequence::Undo);
    if (auto* redo = actions_["edit.redo"]) redo->setShortcut(QKeySequence::Redo);
    if (auto* del = actions_["edit.delete"]) del->setShortcut(QKeySequence::Delete);

    auto* model = ribbon_->addTab(tr("3D Model"));
    auto* primitives = model->addPanel(tr("Create"));
    primitives->addLarge(actions_["feature.box"]);
    primitives->addLarge(actions_["feature.cylinder"]);

    auto* modify = model->addPanel(tr("Modify"));
    modify->addLarge(actions_["feature.cut"]);

    auto* edit = model->addPanel(tr("Edit"));
    edit->addSmall(actions_["edit.undo"]);
    edit->addSmall(actions_["edit.redo"]);
    edit->addSmall(actions_["edit.delete"]);

    auto* view = ribbon_->addTab(tr("View"));
    auto* navigate = view->addPanel(tr("Navigate"));
    navigate->addLarge(actions_["view.fit"]);
    navigate->addLarge(actions_["view.ortho"]);

    // Present but empty, deliberately: the tabs a user expects from Inventor should exist from
    // the start so the shape of the app is visible, rather than appearing later and moving
    // everything. Empty is honest; missing is misleading.
    ribbon_->addTab(tr("Sketch"));
    ribbon_->addTab(tr("Assemble"));
    ribbon_->addTab(tr("Manage"));
    ribbon_->setCurrentTab(0);

    auto* container = new QWidget(this);
    auto* layout = new QVBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(ribbon_);
    setMenuWidget(container);

    auto* fileMenu = menuBar()->addMenu(tr("&File"));
    fileMenu->addAction(tr("E&xit"), QKeySequence::Quit, this, &QWidget::close);
    auto* editMenu = menuBar()->addMenu(tr("&Edit"));
    editMenu->addAction(actions_["edit.undo"]);
    editMenu->addAction(actions_["edit.redo"]);
    editMenu->addSeparator();
    editMenu->addAction(actions_["edit.delete"]);
}

void MainWindow::buildDocks() {
    auto* browserDock = new QDockWidget(tr("Model"), this);
    browserDock->setFeatures(QDockWidget::DockWidgetMovable);
    browser_ = new QTreeWidget(browserDock);
    browser_->setHeaderHidden(true);
    browser_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    browser_->setIndentation(14);
    browserDock->setWidget(browser_);
    addDockWidget(Qt::LeftDockWidgetArea, browserDock);
    resizeDocks({browserDock}, {280}, Qt::Horizontal);

    connect(browser_, &QTreeWidget::itemSelectionChanged, this, [this] {
        if (updatingTree_) return;   // our own write, not a user click
        controller_.clearSelection();
        for (auto* item : browser_->selectedItems()) {
            const auto id = item->data(0, Qt::UserRole).toULongLong();
            controller_.select(cad::document::ObjectId{id}, true);
        }
    });

    auto* propertiesDock = new QDockWidget(tr("Properties"), this);
    propertiesDock->setFeatures(QDockWidget::DockWidgetMovable);
    properties_ = new QTableWidget(propertiesDock);
    properties_->setColumnCount(2);
    properties_->setHorizontalHeaderLabels({tr("Property"), tr("Value")});
    properties_->horizontalHeader()->setStretchLastSection(true);
    properties_->verticalHeader()->setVisible(false);
    properties_->setSelectionBehavior(QAbstractItemView::SelectRows);
    propertiesDock->setWidget(properties_);
    addDockWidget(Qt::RightDockWidgetArea, propertiesDock);
    resizeDocks({propertiesDock}, {300}, Qt::Horizontal);

    connect(properties_, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* item) {
        if (updatingTree_ || item->column() != 1) return;
        const auto ids = controller_.selection();
        if (ids.size() != 1) return;
        const QString name = properties_->item(item->row(), 0)->text();
        // A rejected edit must not leave the wrong text sitting in the cell as if it took.
        if (!controller_.setProperty(ids.front(), name.toStdString(),
                                    item->text().toStdString())) {
            refreshProperties();
        }
    });
}

void MainWindow::buildStatusBar() {
    statusMessage_ = new QLabel(tr("Ready"), this);
    statusStats_ = new QLabel(this);
    statusUnits_ = new QLabel(tr("mm"), this);
    statusBar()->addWidget(statusMessage_, 1);
    statusBar()->addPermanentWidget(statusStats_);
    statusBar()->addPermanentWidget(statusUnits_);
}

void MainWindow::refreshTree() {
    updatingTree_ = true;
    browser_->clear();

    auto* root = new QTreeWidgetItem(browser_);
    root->setText(0, tr("Untitled"));
    root->setExpanded(true);

    for (const auto& item : controller_.tree()) {
        auto* node = new QTreeWidgetItem(root);
        node->setText(0, QString::fromStdString(item.label));
        node->setData(0, Qt::UserRole, QVariant::fromValue<qulonglong>(item.id.value));
        node->setIcon(0, icon(QString::fromStdString(item.type == "Box" ? "box"
                                                    : item.type == "Cylinder" ? "cylinder"
                                                    : item.type == "Cut" ? "cut" : ""),
                              16));
        node->setForeground(0, stateColour(item.state));
        if (!item.error.empty()) {
            node->setToolTip(0, QString::fromStdString(item.error));
            node->setText(0, node->text(0) + QStringLiteral("  ⚠"));
        }
        if (item.selected) node->setSelected(true);
    }
    updatingTree_ = false;
}

void MainWindow::refreshProperties() {
    updatingTree_ = true;
    properties_->setRowCount(0);

    const auto ids = controller_.selection();
    if (ids.size() == 1) {
        const auto rows = controller_.properties(ids.front());
        properties_->setRowCount(static_cast<int>(rows.size()));
        for (int r = 0; r < static_cast<int>(rows.size()); ++r) {
            auto* name = new QTableWidgetItem(QString::fromStdString(rows[std::size_t(r)].name));
            name->setFlags(name->flags() & ~Qt::ItemIsEditable);
            auto* value = new QTableWidgetItem(QString::fromStdString(rows[std::size_t(r)].value));
            if (!rows[std::size_t(r)].editable) {
                value->setFlags(value->flags() & ~Qt::ItemIsEditable);
                value->setForeground(QColor(0x8b, 0x91, 0x9a));
            }
            properties_->setItem(r, 0, name);
            properties_->setItem(r, 1, value);
        }
    }
    updatingTree_ = false;
}

void MainWindow::refreshCommandStates() {
    const auto ctx = controller_.context();
    for (const auto& command : controller_.commands()) {
        const auto it = actions_.find(command.id);
        if (it == actions_.end()) continue;
        it->second->setEnabled(command.enabled ? command.enabled(ctx) : true);
    }
}

void MainWindow::refreshStatus() {
    const auto s = controller_.stats();
    QString text = tr("%1 features").arg(s.objects);
    if (s.instances > 0) {
        text += tr("  ·  %1 mesh(es), %2 instances, %3 triangles")
                    .arg(s.uniqueMeshes)
                    .arg(s.instances)
                    .arg(s.triangles);
    }
    if (s.failed > 0) text += tr("  ·  %1 failed").arg(s.failed);
    statusStats_->setText(text);
}

}  // namespace cadqt
