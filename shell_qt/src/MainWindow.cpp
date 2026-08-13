#include "MainWindow.h"

#include "HomePage.h"
#include "Icons.h"
#include "Ribbon.h"
#include "ViewportPlaceholder.h"

#include <QApplication>
#include <QCloseEvent>
#include <QFileInfo>
#include <QButtonGroup>
#include <QDockWidget>
#include <QFileDialog>
#include <QMessageBox>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QSplitter>
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
    if (type == "Sketch") return QStringLiteral("sketch");
    if (type == "Extrude") return QStringLiteral("extrude");
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
        syncTitle();
    });

    // syncWorkspace, not just the refreshes: it is what hides the docks for Home, and startup
    // begins on Home. Without it the first frame shows an empty Model tree next to the project
    // page, which is exactly the "document failed to load" impression Home should never give.
    syncWorkspace();
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
    addQat(QStringLiteral("open"), tr("Open"), [this] { openDocument(); }, QKeySequence::Open);
    addQat(QStringLiteral("save"), tr("Save"), [this] { saveDocument(false); },
           QKeySequence::Save);
    qatRow->addSpacing(6);
    addQat(QStringLiteral("undo"), tr("Undo"), [this] {
        if (auto* c = controller()) c->undo();
    }, QKeySequence::Undo);
    addQat(QStringLiteral("redo"), tr("Redo"), [this] {
        if (auto* c = controller()) c->redo();
    }, QKeySequence::Redo);

    qatRow->addStretch(1);

    // Selection filter. Non-negotiable in CAD: picking an edge inside a dense assembly is
    // otherwise impossible, and a filter that lives in a preferences dialog is a filter nobody
    // knows is on. It sits at the right of the QAT, visible at all times, because "why did my
    // click select the whole body" is a question the UI should already be answering.
    //
    // The shell owns the setting for now. It becomes a Controller concern the day IPicker
    // resolves a pixel to the nearest entity of a requested TYPE rather than to one element —
    // see DESKTOP_UX.md 3.3, which is a resolution rule over the ID buffer, not new GPU work.
    //
    // Hidden on Home, which has nothing to select. A filter offering to restrict picking on a
    // page with no geometry is chrome pretending to be a control.
    filterBar_ = new QWidget(qat);
    auto* filterRow = new QHBoxLayout(filterBar_);
    filterRow->setContentsMargins(0, 0, 0, 0);
    filterRow->setSpacing(2);

    auto* filterLabel = new QLabel(tr("Select"), filterBar_);
    filterLabel->setObjectName(QStringLiteral("qatFilterLabel"));
    filterRow->addWidget(filterLabel);

    selectionFilter_ = new QButtonGroup(this);
    selectionFilter_->setExclusive(true);
    const std::array<std::pair<const char*, const char*>, 4> filters{{
        {"Body", "Select whole bodies"},
        {"Face", "Select faces"},
        {"Edge", "Select edges"},
        {"Vertex", "Select vertices"},
    }};
    int filterId = 0;
    for (const auto& [name, tip] : filters) {
        auto* button = new QToolButton(filterBar_);
        button->setText(tr(name));
        button->setToolTip(tr(tip));
        button->setCheckable(true);
        button->setAutoRaise(true);
        button->setObjectName(QStringLiteral("qatFilter"));
        if (filterId == 0) button->setChecked(true);
        selectionFilter_->addButton(button, filterId++);
        filterRow->addWidget(button);
    }
    qatRow->addWidget(filterBar_);
    connect(selectionFilter_, &QButtonGroup::idClicked, this, [this](int) { refreshStatus(); });

    qatRow->addSpacing(10);
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
    fileMenu_->addAction(icon(QStringLiteral("open"), 16), tr("Open..."), QKeySequence::Open, this,
                         [this] { openDocument(); });
    fileMenu_->addAction(icon(QStringLiteral("save"), 16), tr("Save"), QKeySequence::Save, this,
                         [this] { saveDocument(false); });
    fileMenu_->addAction(tr("Save As..."), QKeySequence::SaveAs, this,
                         [this] { saveDocument(true); });
    fileMenu_->addSeparator();
    fileMenu_->addAction(tr("Home"), this, [this] { session_.activateHome(); });
    fileMenu_->addSeparator();
    fileMenu_->addAction(tr("Exit"), QKeySequence::Quit, this, &QWidget::close);
}

QAction* MainWindow::command(const char* id) {
    const auto it = actions_.find(id);
    return it == actions_.end() ? nullptr : it->second;
}

QAction* MainWindow::planned(const QString& label, const QString& iconName) {
    // A command the app will have and does not yet: present, disabled, and honest about it.
    //
    // The alternative — showing only what works — makes the app look smaller than it is and
    // makes every later release rearrange the ribbon under the user. Inventor's shape should be
    // legible from the first run, which is the same argument that puts the unimplemented
    // document kinds on the Home page (ADR 0009).
    //
    // Deliberately NOT routed through Controller::commands(): app/ exposes commands that exist.
    // A disabled label is a shell concern.
    auto* action = new QAction(label, this);
    action->setIcon(icon(iconName));
    action->setEnabled(false);
    action->setToolTip(tr("%1 — not implemented yet").arg(label));
    return action;
}

QAction* MainWindow::commandOr(const char* id, const QString& label, const QString& iconName) {
    if (auto* existing = command(id)) return existing;
    return planned(label, iconName);
}

namespace {
/// Shared by Open and Save so the two dialogs cannot drift apart, which is how a user ends up
/// unable to see in Open the file they just saved.
constexpr const char* kDocumentFilter =
    "vCAD documents (*.vpart *.vasm *.vdrw *.vpres);;Part (*.vpart);;All files (*)";
}  // namespace

void MainWindow::openDocument() {
    const QString path = QFileDialog::getOpenFileName(this, tr("Open"), QString(),
                                                      tr(kDocumentFilter));
    if (path.isEmpty()) return;
    openPath(path);
}

void MainWindow::openPath(const QString& path) {
    QApplication::setOverrideCursor(Qt::WaitCursor);
    // Opening recomputes the whole feature tree, which is not instant on a large part.
    const auto result = session_.openDocument(std::filesystem::path(path.toStdString()));
    QApplication::restoreOverrideCursor();

    if (!result) {
        QMessageBox::warning(this, tr("Could not open"),
                             QString::fromStdString(result.error().message), QMessageBox::Ok);
        statusMessage_->setText(tr("Open failed"));
        return;
    }
    // Session::openDocument fires its changed callback, which rebuilds the tabs and the ribbon.
    statusMessage_->setText(tr("Opened %1").arg(QFileInfo(path).fileName()));
}

bool MainWindow::saveDocument(bool saveAs) {
    if (session_.homeActive() || session_.active() == nullptr) {
        statusMessage_->setText(tr("There is no document to save"));
        return false;
    }

    std::filesystem::path target = session_.activePath();
    if (saveAs || target.empty()) {
        // Seed the dialog with the document's title and the right extension, so Save on a new part
        // is one keystroke rather than a naming exercise.
        const auto& doc = session_.documents()[session_.activeIndex()];
        const QString suggested =
            QString::fromStdString(doc.title + cad::app::fileExtension(doc.kind));
        const QString path =
            QFileDialog::getSaveFileName(this, tr("Save As"), suggested, tr(kDocumentFilter));
        if (path.isEmpty()) return false;
        target = std::filesystem::path(path.toStdString());
    }

    const auto result = session_.saveActive(target);
    if (!result) {
        QMessageBox::warning(this, tr("Could not save"),
                             QString::fromStdString(result.error().message), QMessageBox::Ok);
        statusMessage_->setText(tr("Save failed"));
        return false;
    }
    syncTitle();
    return true;
}

/// Returns false if the user cancelled, i.e. "do not proceed with whatever prompted this".
bool MainWindow::confirmDiscardChanges() {
    if (!session_.activeModified()) return true;

    const auto& doc = session_.documents()[session_.activeIndex()];
    const auto choice = QMessageBox::warning(
        this, tr("Unsaved changes"),
        tr("Save changes to %1?").arg(QString::fromStdString(doc.title)),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);

    if (choice == QMessageBox::Cancel) return false;
    if (choice == QMessageBox::Discard) return true;
    // Save may itself be cancelled at the file dialog, which must cancel the close too — otherwise
    // "Save" silently behaves as "Discard", which is the worst possible outcome of this dialog.
    return saveDocument(false);
}

void MainWindow::syncTitle() {
    QString title = QStringLiteral("vCAD");
    if (!session_.homeActive() && session_.activeIndex() < session_.count()) {
        const auto& doc = session_.documents()[session_.activeIndex()];
        title += QStringLiteral(" — ") + QString::fromStdString(doc.title);
        // The asterisk is the platform-neutral dirty marker. Cheap, universally understood, and
        // the only signal a user has that closing will cost them something.
        if (session_.activeModified()) title += QStringLiteral("*");
    }
    setWindowTitle(title);
}

void MainWindow::closeEvent(QCloseEvent* event) {
    // Every open document, not just the active one: quitting must not silently drop edits in a tab
    // the user is not looking at.
    for (std::size_t i = 0; i < session_.count(); ++i) {
        session_.activate(i);
        if (!confirmDiscardChanges()) {
            event->ignore();
            return;
        }
    }
    event->accept();
}

void MainWindow::importFile() {
    auto* c = controller();
    if (c == nullptr) return;

    // Filters name the formats core/io actually registers, and nothing else. A dialog offering
    // .dxf or .obj that then fails to read them is worse than not listing them.
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Import"), QString(),
        tr("CAD files (*.step *.stp *.iges *.igs *.stl);;"
           "STEP (*.step *.stp);;IGES (*.iges *.igs);;STL (*.stl);;All files (*)"));
    if (path.isEmpty()) return;

    // Reading a large STEP file is not instant and blocks this thread. An hourglass is the
    // honest minimum; moving import off the UI thread is its own piece of work.
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const auto result = c->importFile(std::filesystem::path(path.toStdString()));
    QApplication::restoreOverrideCursor();

    if (!result) {
        // The kernel's message is written for a user, so show it rather than a generic failure.
        QMessageBox::warning(this, tr("Import failed"),
                             QString::fromStdString(result.error().message),
                             QMessageBox::Ok);
        statusMessage_->setText(tr("Import failed"));
        return;
    }
    refreshTree();
    refreshCommandStates();
}

void MainWindow::rebuildRibbon() {
    // Tabs are a function of the active workspace. Home contributes none of its own.
    ribbon_->clearTabs();
    actions_.clear();

    auto* c = controller();
    if (c == nullptr) {
        // Home's ribbon, matching Inventor: application-level tabs only. Creating documents is
        // the Home page's own job — the sidebar's New... button — so the ribbon does not
        // duplicate it.
        auto* tools = ribbon_->addTab(tr("Tools"));
        auto* options = tools->addPanel(tr("Options"));
        options->addLarge(planned(tr("Application\nOptions"), QStringLiteral("parameters")));
        options->addLarge(planned(tr("Document\nSettings"), QStringLiteral("note")));
        auto* cachePanel = tools->addPanel(tr("Cache"));
        cachePanel->addLarge(planned(tr("Cache\nStatus"), QStringLiteral("cache")));
        cachePanel->addSmall(planned(tr("Purge Local"), QStringLiteral("purge")));

        auto* collaborate = ribbon_->addTab(tr("Collaborate"));
        auto* project = collaborate->addPanel(tr("Project"));
        project->addLarge(planned(tr("Projects"), QStringLiteral("assembly")));
        project->addSmall(planned(tr("Search Paths"), QStringLiteral("open")));

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
    if (auto* undo = command("edit.undo")) undo->setShortcut(QKeySequence::Undo);
    if (auto* redo = command("edit.redo")) redo->setShortcut(QKeySequence::Redo);
    if (auto* del = command("edit.delete")) del->setShortcut(QKeySequence::Delete);

    // Only the Part/model tab set exists, because Part is the only implemented document kind
    // (ADR 0009). Assembly and Drawing get their own sets here when their documents open.

    // ── 3D Model ────────────────────────────────────────────────────────────────────────
    auto* model = ribbon_->addTab(tr("3D Model"));

    auto* sketchPanel = model->addPanel(tr("Sketch"));
    sketchPanel->addLarge(commandOr("feature.sketch", tr("Start\nSketch"),
                                    QStringLiteral("sketch")));

    // Create and Modify are Inventor's, command for command and in Inventor's order — two large
    // buttons then small ones stacked three per column, which is why the column breaks below fall
    // where they do. Read down each column, not across:
    //
    //   Create   Extrude Revolve | Sweep  Emboss Decal
    //                            | Loft   Derive Import
    //                            | Coil   Rib    Unwrap
    //
    //   Modify   Hole    Fillet  | Chamfer Thread          Split       Mark
    //                            | Shell   Combine         Direct      Finish
    //                            | Draft   Thicken/Offset  Delete Face
    //
    // Almost all of it is disabled, and that is the point: the tab shows what a Part document is
    // for, so nothing moves when the commands arrive. Inventor's own layout is the specification
    // here — we are not designing a ribbon, we are copying one people already know.
    auto* create = model->addPanel(tr("Create"));
    create->addLarge(commandOr("feature.extrude", tr("Extrude"), QStringLiteral("extrude")));
    create->addLarge(planned(tr("Revolve"), QStringLiteral("revolve")));
    create->addSmall(planned(tr("Sweep"), QStringLiteral("sweep")));
    create->addSmall(planned(tr("Loft"), QStringLiteral("loft")));
    create->addSmall(planned(tr("Coil"), QStringLiteral("coil")));
    create->addSmall(planned(tr("Emboss"), QStringLiteral("emboss")));
    create->addSmall(planned(tr("Derive"), QStringLiteral("derive")));
    create->addSmall(planned(tr("Rib"), QStringLiteral("rib")));
    create->addSmall(planned(tr("Decal"), QStringLiteral("decal")));
    // Import resolves to a command the day one is registered. The capability already exists —
    // core/io reads STEP, IGES and STL, and FeatureRegistry::builtins() has an Import feature —
    // but no Controller command wraps it yet, so commandOr falls back to disabled.
    // Import is a shell action rather than a Controller command, because it needs a file dialog
    // and app/ must stay toolkit-free for the iPad shell. The command registry deliberately has
    // no way to pass a parameter; anything needing one is wired like this.
    {
        auto* action = new QAction(icon(QStringLiteral("import")), tr("Import"), this);
        action->setToolTip(tr("Import a STEP, IGES or STL file into this part"));
        connect(action, &QAction::triggered, this, &MainWindow::importFile);
        create->addSmall(action);
    }
    create->addSmall(planned(tr("Unwrap"), QStringLiteral("unwrap")));

    // Ours, not Inventor's, and deliberately a SEPARATE panel rather than smuggled into Create.
    //
    // Inventor has no primitive solids in the 3D Model tab: every solid starts as a sketch, so
    // Create begins at Extrude. We cannot sketch yet, which would leave a Part document with no
    // way to make geometry at all. Keeping them in their own panel means Create still matches the
    // reference exactly, and this panel disappears the day Sketch + Extrude work rather than
    // leaving a permanent wart inside a panel we are supposed to be copying.
    auto* primitives = model->addPanel(tr("Primitives"));
    primitives->addLarge(commandOr("feature.box", tr("Box"), QStringLiteral("box")));
    primitives->addLarge(commandOr("feature.cylinder", tr("Cylinder"), QStringLiteral("cylinder")));

    auto* modify = model->addPanel(tr("Modify"));
    modify->addLarge(planned(tr("Hole"), QStringLiteral("hole")));
    modify->addLarge(commandOr("feature.fillet", tr("Fillet"), QStringLiteral("fillet")));
    modify->addSmall(commandOr("feature.chamfer", tr("Chamfer"), QStringLiteral("chamfer")));
    modify->addSmall(planned(tr("Shell"), QStringLiteral("shell")));
    modify->addSmall(planned(tr("Draft"), QStringLiteral("draft")));
    modify->addSmall(planned(tr("Thread"), QStringLiteral("thread")));
    // Inventor's Combine IS the boolean between solid bodies, and Join / Cut / Intersect are its
    // three modes — exactly our Fuse, Cut and Common. All three are implemented, so the button
    // gets a drop-down rather than silently exposing only one of them. Inventor puts drop-downs on
    // ribbon buttons too (its own Fillet has one), so this is copying the reference, not diverging
    // from it.
    if (auto* combine = modify->addSmall(
            commandOr("feature.cut", tr("Combine"), QStringLiteral("combine")))) {
        auto* modes = new QMenu(combine);
        for (const auto& [id, label] : {std::pair{"feature.cut", tr("Cut")},
                                        std::pair{"feature.fuse", tr("Join")},
                                        std::pair{"feature.common", tr("Intersect")}}) {
            if (auto* action = command(id)) {
                action->setText(label);
                modes->addAction(action);
            }
        }
        if (!modes->isEmpty()) {
            combine->setMenu(modes);
            combine->setPopupMode(QToolButton::MenuButtonPopup);
        }
    }
    modify->addSmall(planned(tr("Thicken/Offset"), QStringLiteral("thicken")));
    modify->addSmall(planned(tr("Split"), QStringLiteral("split")));
    modify->addSmall(planned(tr("Direct"), QStringLiteral("direct")));
    modify->addSmall(planned(tr("Delete Face"), QStringLiteral("delete-face")));
    modify->addSmall(planned(tr("Mark"), QStringLiteral("mark")));
    modify->addSmall(planned(tr("Finish"), QStringLiteral("finish")));

    auto* pattern = model->addPanel(tr("Pattern"));
    pattern->addSmall(planned(tr("Rectangular"), QStringLiteral("pattern-rect")));
    pattern->addSmall(planned(tr("Circular"), QStringLiteral("pattern-circular")));
    pattern->addSmall(planned(tr("Mirror"), QStringLiteral("mirror")));

    auto* editPanel = model->addPanel(tr("Edit"));
    editPanel->addSmall(commandOr("edit.undo", tr("Undo"), QStringLiteral("undo")));
    editPanel->addSmall(commandOr("edit.redo", tr("Redo"), QStringLiteral("redo")));
    editPanel->addSmall(commandOr("edit.delete", tr("Delete"), QStringLiteral("delete")));

    // Rollback gets its own panel rather than sitting in Edit: it is not an edit at all -- it moves
    // where you are in the tree, and grouping it with Undo/Delete would suggest otherwise.
    auto* historyPanel = model->addPanel(tr("History"));
    historyPanel->addSmall(commandOr("edit.rollback", tr("Roll Back"),
                                     QStringLiteral("rollback")));
    historyPanel->addSmall(commandOr("edit.rollforward", tr("Roll Forward"),
                                     QStringLiteral("rollforward")));

    // ── Sketch ──────────────────────────────────────────────────────────────────────────
    auto* sketch = ribbon_->addTab(tr("Sketch"));
    auto* sketchManage = sketch->addPanel(tr("Manage"));
    sketchManage->addLarge(commandOr("feature.sketch", tr("Start\nSketch"),
                                     QStringLiteral("sketch")));
    sketchManage->addLarge(planned(tr("Edit\nSketch"), QStringLiteral("sketch-edit")));
    sketchManage->addLarge(planned(tr("Delete\nSketch"), QStringLiteral("delete")));

    // ── Inspect ─────────────────────────────────────────────────────────────────────────
    auto* inspect = ribbon_->addTab(tr("Inspect"));
    auto* measure = inspect->addPanel(tr("Measure"));
    measure->addLarge(planned(tr("Measure"), QStringLiteral("measure")));
    auto* analysis = inspect->addPanel(tr("Analysis"));
    analysis->addLarge(planned(tr("Section\nView"), QStringLiteral("section")));
    analysis->addSmall(planned(tr("Mass Properties"), QStringLiteral("mass")));
    analysis->addSmall(planned(tr("Draft Analysis"), QStringLiteral("draft")));

    // ── Annotate ────────────────────────────────────────────────────────────────────────
    auto* annotate = ribbon_->addTab(tr("Annotate"));
    auto* annotation = annotate->addPanel(tr("3D Annotation"));
    annotation->addLarge(planned(tr("Dimension"), QStringLiteral("dimension")));
    annotation->addLarge(planned(tr("Note"), QStringLiteral("note")));

    // ── Manage ──────────────────────────────────────────────────────────────────────────
    auto* manage = ribbon_->addTab(tr("Manage"));
    auto* parameters = manage->addPanel(tr("Parameters"));
    parameters->addLarge(planned(tr("Parameters"), QStringLiteral("parameters")));
    // The DDC, surfaced in the UI. No other CAD application has this panel because no other CAD
    // application has a content-addressed recompute cache (ADR 0004).
    auto* cache = manage->addPanel(tr("Cache"));
    cache->addLarge(planned(tr("Cache\nStatus"), QStringLiteral("cache")));
    cache->addSmall(planned(tr("Purge Local"), QStringLiteral("purge")));

    // ── View ────────────────────────────────────────────────────────────────────────────
    auto* view = ribbon_->addTab(tr("View"));
    auto* navigate = view->addPanel(tr("Navigate"));
    navigate->addLarge(commandOr("view.fit", tr("Fit"), QStringLiteral("fit")));
    navigate->addLarge(commandOr("view.ortho", tr("Ortho"), QStringLiteral("ortho")));
    auto* appearance = view->addPanel(tr("Appearance"));
    appearance->addSmall(planned(tr("Shaded"), QStringLiteral("shaded")));
    appearance->addSmall(planned(tr("Shaded + Edges"), QStringLiteral("shaded-edges")));
    appearance->addSmall(planned(tr("Wireframe"), QStringLiteral("wireframe")));
    auto* visibility = view->addPanel(tr("Visibility"));
    visibility->addSmall(planned(tr("Origin Planes"), QStringLiteral("origin")));
    visibility->addSmall(planned(tr("All Sketches"), QStringLiteral("sketches")));

    ribbon_->setCurrentTab(0);
    refreshCommandStates();
}

// ── workspaces ──────────────────────────────────────────────────────────────────────────

void MainWindow::buildWorkspaces() {
    // Two columns: Home's rail, then everything else stacked over the document tabs.
    //
    // The tab bar is deliberately INSIDE the right column rather than spanning the window. With
    // one full-width bar the rail stopped above it and read as a panel sitting inside the page;
    // owning the left column down to the status bar is what makes it a sidebar.
    // A QSplitter rather than a plain layout, so the rail can be dragged. The handle is the
    // divider the user grabs; the rail's own min/max width bound how far it can go, which is why
    // those live on the widget instead of here.
    auto* centre = new QWidget(this);
    auto* columns = new QHBoxLayout(centre);
    columns->setContentsMargins(0, 0, 0, 0);
    columns->setSpacing(0);

    homeSplitter_ = new QSplitter(Qt::Horizontal, centre);
    homeSplitter_->setObjectName(QStringLiteral("homeSplitter"));
    homeSplitter_->setChildrenCollapsible(false);   // dragging must not make the rail vanish
    homeSplitter_->setHandleWidth(4);               // 1px reads as a border and cannot be grabbed
    columns->addWidget(homeSplitter_, 1);

    workspaces_ = new QStackedWidget(homeSplitter_);
    home_ = new HomePage(session_, workspaces_);
    workspaces_->addWidget(home_);

    homeSidebar_ = home_->sidebar();
    homeSplitter_->addWidget(homeSidebar_);

    auto* right = new QWidget(homeSplitter_);
    auto* layout = new QVBoxLayout(right);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(workspaces_, 1);
    homeSplitter_->addWidget(right);

    // Only the content column absorbs window resizing; the rail keeps whatever width the user
    // dragged it to, which is what every CAD sidebar does.
    homeSplitter_->setStretchFactor(0, 0);
    homeSplitter_->setStretchFactor(1, 1);
    homeSplitter_->setSizes({HomePage::sidebarDefaultWidth(), 1});

    connect(home_, &HomePage::openBrowseRequested, this, [this] { openDocument(); });
    connect(home_, &HomePage::openRequested, this,
            [this](const QString& path) { openPath(path); });
    connect(home_, &HomePage::createRequested, this,
            [this](int kind) { createDocument(static_cast<DocumentKind>(kind)); });

    // Document tabs along the BOTTOM, as Inventor does.
    documentTabs_ = new QTabBar(right);
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

void MainWindow::selectRibbonTab(int index) {
    if (ribbon_ != nullptr) ribbon_->setCurrentTab(index);
}

void MainWindow::openDemoDocument() {
    createDocument(DocumentKind::Part);
    auto* c = controller();
    if (c == nullptr) return;
    // Two features, so the browser has depth and the properties panel has something to show.
    for (const char* id : {"feature.box", "feature.cylinder"}) {
        for (const auto& command : c->commands()) {
            if (command.id == id) { command.invoke(); break; }
        }
    }
    refreshTree();
    refreshProperties();
    refreshCommandStates();
    refreshStatus();
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
        // The title's dirty marker is derived from the document, so it has to be refreshed on
        // every edit — not only when the active document changes.
        syncTitle();
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
    // Home fills the window. Inventor hides the browser and the property panel there, and it is
    // right to: neither has anything to show, and an empty Model tree beside a project page reads
    // as a document that failed to load. They come back with the first document.
    const bool home = session_.homeActive();
    if (browserDock_ != nullptr) browserDock_->setVisible(!home);
    if (propertiesDock_ != nullptr) propertiesDock_->setVisible(!home);
    if (filterBar_ != nullptr) filterBar_->setVisible(!home);
    if (homeSidebar_ != nullptr) homeSidebar_->setVisible(home);

    if (home) {
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
    browserDock_ = new QDockWidget(tr("Model"), this);
    auto* browserDock = browserDock_;
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

    propertiesDock_ = new QDockWidget(tr("Properties"), this);
    auto* propertiesDock = propertiesDock_;
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
