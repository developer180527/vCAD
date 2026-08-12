#include "MainWindow.h"

#include "HomePage.h"
#include "Icons.h"
#include "Ribbon.h"
#include "ViewportPlaceholder.h"

#include <QDockWidget>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTabBar>
#include <QTableWidget>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace cadqt {
namespace {

using cad::app::DocumentKind;

/// Per-state colour for the browser. A failed feature must be obvious at a glance — FreeCAD marks
/// errors subtly enough that people miss them and then wonder why the model is wrong.
QColor stateColour(cad::document::ObjectState state) {
    switch (state) {
        case cad::document::ObjectState::Clean:   return QColor(0x1f, 0x21, 0x24);
        case cad::document::ObjectState::Dirty:   return QColor(0xa8, 0x6a, 0x00);
        case cad::document::ObjectState::Failed:  return QColor(0xc0, 0x2a, 0x24);
        case cad::document::ObjectState::Blocked: return QColor(0x8a, 0x6d, 0x3f);
    }
    return QColor(0x1f, 0x21, 0x24);
}

QString iconNameFor(const std::string& type) {
    if (type == "Box") return QStringLiteral("box");
    if (type == "Cylinder") return QStringLiteral("cylinder");
    if (type == "Cut") return QStringLiteral("cut");
    return {};
}

}  // namespace

MainWindow::MainWindow() {
    setWindowTitle(tr("vCAD"));
    resize(1500, 940);

    buildTopArea();
    buildDocks();
    buildWorkspaces();
    buildStatusBar();

    session_.onChanged([this] {
        syncWorkspace();
        refreshDocumentTabs();
        rebuildRibbon();
        refreshTree();
        refreshProperties();
        refreshCommandStates();
        refreshStatus();
    });

    rebuildRibbon();
    refreshDocumentTabs();
    refreshCommandStates();
    refreshStatus();
}

MainWindow::~MainWindow() = default;

// ── top area ────────────────────────────────────────────────────────────────────────────

void MainWindow::buildTopArea() {
    // ONE menu widget holding both strips.
    //
    // The previous version called setMenuWidget(ribbon) and then menuBar(), which REPLACES the
    // menu widget — so the ribbon was destroyed at startup and, because macOS puts QMenuBar in the
    // global bar, the window showed no commands at all. There is no QMenuBar here now: Inventor
    // has none either, it has a File tab in the ribbon strip.
    auto* top = new QWidget(this);
    auto* topLayout = new QVBoxLayout(top);
    topLayout->setContentsMargins(0, 0, 0, 0);
    topLayout->setSpacing(0);

    // Quick access toolbar, the strip above the ribbon.
    auto* qat = new QWidget(top);
    qat->setObjectName(QStringLiteral("qat"));
    auto* qatRow = new QHBoxLayout(qat);
    qatRow->setContentsMargins(0, 0, 8, 0);
    qatRow->setSpacing(2);

    auto* fileTab = new QToolButton(qat);
    fileTab->setText(tr("File"));
    fileTab->setObjectName(QStringLiteral("fileTab"));
    fileTab->setPopupMode(QToolButton::InstantPopup);
    fileMenu_ = new QMenu(fileTab);
    fileTab->setMenu(fileMenu_);
    qatRow->addWidget(fileTab);
    qatRow->addSpacing(6);

    const auto addQat = [&](const QString& iconName, const QString& text,
                            const std::function<void()>& fn, const QKeySequence& shortcut) {
        auto* button = new QToolButton(qat);
        button->setIcon(icon(iconName, 18));
        button->setIconSize(QSize(18, 18));
        button->setToolTip(shortcut.isEmpty() ? text
                                              : text + QStringLiteral(" (")
                                                    + shortcut.toString(QKeySequence::NativeText)
                                                    + QStringLiteral(")"));
        button->setObjectName(QStringLiteral("qatButton"));
        button->setAutoRaise(true);
        connect(button, &QToolButton::clicked, this, fn);
        qatRow->addWidget(button);
        return button;
    };

    addQat(QStringLiteral("new"), tr("New part"),
           [this] { createDocument(DocumentKind::Part); }, QKeySequence::New);
    addQat(QStringLiteral("open"), tr("Open"), [this] {
        statusMessage_->setText(tr("Opening files is not implemented yet"));
    }, QKeySequence::Open);
    addQat(QStringLiteral("save"), tr("Save"), [this] {
        statusMessage_->setText(tr("Saving is not implemented yet"));
    }, QKeySequence::Save);
    qatRow->addSpacing(6);
    addQat(QStringLiteral("undo"), tr("Undo"), [this] {
        if (auto* c = controller()) c->undo();
    }, QKeySequence::Undo);
    addQat(QStringLiteral("redo"), tr("Redo"), [this] {
        if (auto* c = controller()) c->redo();
    }, QKeySequence::Redo);

    qatRow->addStretch(1);
    auto* product = new QLabel(tr("vCAD"), qat);
    product->setStyleSheet(QStringLiteral("color: #6c7075;"));
    qatRow->addWidget(product);
    topLayout->addWidget(qat);

    ribbon_ = new Ribbon(top);
    topLayout->addWidget(ribbon_);
    setMenuWidget(top);

    // File menu, on the File tab rather than a menu bar.
    fileMenu_->addAction(icon(QStringLiteral("part"), 16), tr("New Part"), this,
                         [this] { createDocument(DocumentKind::Part); });
    for (const auto kind : {DocumentKind::Assembly, DocumentKind::Drawing,
                            DocumentKind::Presentation}) {
        auto* action = fileMenu_->addAction(
            icon(QString::fromUtf8(cad::app::toString(kind)).toLower(), 16),
            tr("New %1").arg(QString::fromUtf8(cad::app::toString(kind))));
        // Present but disabled: a user should see the app's intended shape, not wonder whether
        // assemblies exist.
        action->setEnabled(cad::app::implemented(kind));
    }
    fileMenu_->addSeparator();
    fileMenu_->addAction(tr("Home"), this, [this] { session_.activateHome(); });
    fileMenu_->addSeparator();
    fileMenu_->addAction(tr("Exit"), QKeySequence::Quit, this, &QWidget::close);
}

void MainWindow::rebuildRibbon() {
    // Tabs are a function of the active workspace. Home contributes none of its own.
    ribbon_->clearTabs();
    actions_.clear();

    auto* c = controller();
    if (c == nullptr) {
        auto* start = ribbon_->addTab(tr("Get Started"));
        auto* panel = start->addPanel(tr("New"));
        for (const auto kind : {DocumentKind::Part, DocumentKind::Assembly,
                                DocumentKind::Drawing}) {
            auto* action = new QAction(QString::fromUtf8(cad::app::toString(kind)), this);
            action->setIcon(icon(QString::fromUtf8(cad::app::toString(kind)).toLower()));
            action->setEnabled(cad::app::implemented(kind));
            connect(action, &QAction::triggered, this, [this, kind] { createDocument(kind); });
            panel->addLarge(action);
        }
        ribbon_->setCurrentTab(0);
        return;
    }

    for (const auto& command : c->commands()) {
        auto* action = new QAction(QString::fromStdString(command.label), this);
        action->setToolTip(QString::fromStdString(command.tooltip));
        action->setIcon(icon(QString::fromStdString(command.iconName)));
        const auto invoke = command.invoke;
        connect(action, &QAction::triggered, this, [invoke] { invoke(); });
        actions_[command.id] = action;
    }
    if (auto* undo = actions_["edit.undo"]) undo->setShortcut(QKeySequence::Undo);
    if (auto* redo = actions_["edit.redo"]) redo->setShortcut(QKeySequence::Redo);
    if (auto* del = actions_["edit.delete"]) del->setShortcut(QKeySequence::Delete);

    auto* model = ribbon_->addTab(tr("3D Model"));
    auto* create = model->addPanel(tr("Create"));
    create->addLarge(actions_["feature.box"]);
    create->addLarge(actions_["feature.cylinder"]);
    auto* modify = model->addPanel(tr("Modify"));
    modify->addLarge(actions_["feature.cut"]);
    auto* editPanel = model->addPanel(tr("Edit"));
    editPanel->addSmall(actions_["edit.undo"]);
    editPanel->addSmall(actions_["edit.redo"]);
    editPanel->addSmall(actions_["edit.delete"]);

    auto* view = ribbon_->addTab(tr("View"));
    auto* navigate = view->addPanel(tr("Navigate"));
    navigate->addLarge(actions_["view.fit"]);
    navigate->addLarge(actions_["view.ortho"]);

    // Empty but present, so the app's shape is visible from the start instead of shifting later.
    ribbon_->addTab(tr("Sketch"));
    ribbon_->addTab(tr("Inspect"));
    ribbon_->addTab(tr("Manage"));
    ribbon_->setCurrentTab(0);
    refreshCommandStates();
}

// ── workspaces ──────────────────────────────────────────────────────────────────────────

void MainWindow::buildWorkspaces() {
    auto* centre = new QWidget(this);
    auto* layout = new QVBoxLayout(centre);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    workspaces_ = new QStackedWidget(centre);
    home_ = new HomePage(session_, workspaces_);
    workspaces_->addWidget(home_);
    layout->addWidget(workspaces_, 1);

    connect(home_, &HomePage::createRequested, this,
            [this](int kind) { createDocument(static_cast<DocumentKind>(kind)); });

    // Document tabs along the BOTTOM, as Inventor does.
    documentTabs_ = new QTabBar(centre);
    documentTabs_->setObjectName(QStringLiteral("docTabs"));
    documentTabs_->setExpanding(false);
    documentTabs_->setDrawBase(false);
    documentTabs_->setShape(QTabBar::RoundedSouth);
    layout->addWidget(documentTabs_, 0);

    connect(documentTabs_, &QTabBar::currentChanged, this, [this](int index) {
        if (updatingUi_ || index < 0) return;
        // Tab 0 is Home, which is not a document — hence index-1 into the document list.
        if (index == 0) {
            session_.activateHome();
        } else {
            session_.activate(static_cast<std::size_t>(index - 1));
        }
    });
    connect(documentTabs_, &QTabBar::tabCloseRequested, this, [this](int index) {
        if (index <= 0) return;   // Home cannot be closed
        const auto docIndex = static_cast<std::size_t>(index - 1);
        auto* editor = editors_[docIndex];
        editors_.erase(editors_.begin() + static_cast<std::ptrdiff_t>(docIndex));
        workspaces_->removeWidget(editor);
        editor->deleteLater();
        session_.close(docIndex);
    });

    setCentralWidget(centre);
}

void MainWindow::createDocument(DocumentKind kind) {
    if (!cad::app::implemented(kind)) {
        statusMessage_->setText(
            tr("%1 documents are not implemented yet").arg(QString::fromUtf8(
                cad::app::toString(kind))));
        return;
    }
    const std::size_t index = session_.create(kind);
    auto* c = session_.documents()[index].controller.get();

    auto* editor = new ViewportPlaceholder(*c, workspaces_);
    workspaces_->addWidget(editor);
    editors_.push_back(editor);

    // Each document's controller drives the shared panels, but only while it is the active one —
    // otherwise a background recompute would repaint another document's tree.
    c->onDocumentChanged([this, c] {
        if (controller() != c) return;
        refreshTree();
        refreshProperties();
        refreshCommandStates();
        refreshStatus();
        if (auto* w = workspaces_->currentWidget()) w->update();
    });
    c->onViewChanged([this, c] {
        if (controller() != c) return;
        if (auto* w = workspaces_->currentWidget()) w->update();
        refreshStatus();
    });
    c->onStatus([this, c](const std::string& text) {
        if (controller() != c) return;
        statusMessage_->setText(QString::fromStdString(text));
    });

    syncWorkspace();
    refreshDocumentTabs();
    rebuildRibbon();
}

void MainWindow::syncWorkspace() {
    if (session_.homeActive()) {
        workspaces_->setCurrentWidget(home_);
        home_->refresh();
        setWindowTitle(tr("vCAD"));
        return;
    }
    const std::size_t index = session_.activeIndex();
    if (index < editors_.size()) workspaces_->setCurrentWidget(editors_[index]);
    if (index < session_.count()) {
        setWindowTitle(tr("vCAD — %1")
                           .arg(QString::fromStdString(session_.documents()[index].title)));
    }
}

void MainWindow::refreshDocumentTabs() {
    updatingUi_ = true;
    while (documentTabs_->count() > 0) documentTabs_->removeTab(0);

    documentTabs_->addTab(tr("Home"));
    for (const auto& doc : session_.documents()) {
        const int i = documentTabs_->addTab(QString::fromStdString(doc.title));
        documentTabs_->setTabIcon(
            i, icon(QString::fromUtf8(cad::app::toString(doc.kind)).toLower(), 14));
    }
    documentTabs_->setTabsClosable(true);
    // Home has no close button, because it cannot be closed (ADR 0009).
    if (auto* button = documentTabs_->tabButton(0, QTabBar::RightSide)) {
        button->hide();
    }
    documentTabs_->setCurrentIndex(session_.homeActive()
                                       ? 0
                                       : static_cast<int>(session_.activeIndex()) + 1);
    updatingUi_ = false;
}

// ── docks ───────────────────────────────────────────────────────────────────────────────

void MainWindow::buildDocks() {
    auto* browserDock = new QDockWidget(tr("Model"), this);
    browserDock->setFeatures(QDockWidget::DockWidgetMovable);
    browser_ = new QTreeWidget(browserDock);
    browser_->setHeaderHidden(true);
    browser_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    browser_->setIndentation(14);
    browserDock->setWidget(browser_);
    addDockWidget(Qt::LeftDockWidgetArea, browserDock);
    resizeDocks({browserDock}, {290}, Qt::Horizontal);

    connect(browser_, &QTreeWidget::itemSelectionChanged, this, [this] {
        if (updatingUi_) return;
        auto* c = controller();
        if (c == nullptr) return;
        c->clearSelection();
        for (auto* item : browser_->selectedItems()) {
            const auto id = item->data(0, Qt::UserRole).toULongLong();
            if (id != 0) c->select(cad::document::ObjectId{id}, true);
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
        if (updatingUi_ || item->column() != 1) return;
        auto* c = controller();
        if (c == nullptr) return;
        const auto ids = c->selection();
        if (ids.size() != 1) return;
        const QString name = properties_->item(item->row(), 0)->text();
        // A rejected edit must not leave the wrong text in the cell as if it took.
        if (!c->setProperty(ids.front(), name.toStdString(), item->text().toStdString())) {
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

// ── refresh ─────────────────────────────────────────────────────────────────────────────

void MainWindow::refreshTree() {
    updatingUi_ = true;
    browser_->clear();

    auto* c = controller();
    if (c != nullptr) {
        auto* root = new QTreeWidgetItem(browser_);
        const std::size_t index = session_.activeIndex();
        root->setText(0, QString::fromStdString(session_.documents()[index].title));
        root->setExpanded(true);

        for (const auto& item : c->tree()) {
            auto* node = new QTreeWidgetItem(root);
            node->setText(0, QString::fromStdString(item.label));
            node->setData(0, Qt::UserRole, QVariant::fromValue<qulonglong>(item.id.value));
            node->setIcon(0, icon(iconNameFor(item.type), 16));
            node->setForeground(0, stateColour(item.state));
            if (!item.error.empty()) {
                node->setToolTip(0, QString::fromStdString(item.error));
                node->setText(0, node->text(0) + QStringLiteral("  ⚠"));
            }
            if (item.selected) node->setSelected(true);
        }
    }
    updatingUi_ = false;
}

void MainWindow::refreshProperties() {
    updatingUi_ = true;
    properties_->setRowCount(0);

    auto* c = controller();
    if (c != nullptr) {
        const auto ids = c->selection();
        if (ids.size() == 1) {
            const auto rows = c->properties(ids.front());
            properties_->setRowCount(static_cast<int>(rows.size()));
            for (int r = 0; r < static_cast<int>(rows.size()); ++r) {
                const auto& row = rows[static_cast<std::size_t>(r)];
                auto* name = new QTableWidgetItem(QString::fromStdString(row.name));
                name->setFlags(name->flags() & ~Qt::ItemIsEditable);
                auto* value = new QTableWidgetItem(QString::fromStdString(row.value));
                if (!row.editable) {
                    value->setFlags(value->flags() & ~Qt::ItemIsEditable);
                    value->setForeground(QColor(0x84, 0x88, 0x8d));
                }
                properties_->setItem(r, 0, name);
                properties_->setItem(r, 1, value);
            }
        }
    }
    updatingUi_ = false;
}

void MainWindow::refreshCommandStates() {
    auto* c = controller();
    if (c == nullptr) return;
    const auto ctx = c->context();
    for (const auto& command : c->commands()) {
        const auto it = actions_.find(command.id);
        if (it == actions_.end()) continue;
        it->second->setEnabled(command.enabled ? command.enabled(ctx) : true);
    }
}

void MainWindow::refreshStatus() {
    auto* c = controller();
    if (c == nullptr) {
        statusStats_->setText(session_.count() == 0
                                  ? tr("No documents open")
                                  : tr("%1 document(s) open").arg(session_.count()));
        return;
    }
    const auto s = c->stats();
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
