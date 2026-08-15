#include "MainWindow.h"

#include "HomePage.h"
#include "Icons.h"
#include "proshell/Ribbon.h"
#include "SketchCanvas.h"
#include "Viewport.h"

#include <QApplication>
#include <QCloseEvent>
#include <QFileInfo>
#include <QButtonGroup>
#include <QDockWidget>
#include <QFileDialog>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <algorithm>
#include <set>
#include <QInputDialog>
#include <QLineEdit>
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
        // GREY, not red. The document is intact and this machine is missing software; colouring
        // it like a failure is what makes a user conclude a colleague's file is corrupt.
        case cad::document::ObjectState::NeedsPlugin: return QColor(0x8b, 0x8f, 0x94);
    }
    return QColor(0x1f, 0x21, 0x24);
}

/// Short state badge for the tree's second column.
///
/// Text, not just colour. Blocked and Failed read very differently to a user: Failed means "this
/// feature is wrong", Blocked means "something above it is wrong and this never got a chance",
/// and telling them apart is the difference between fixing the right feature and the wrong one.
QString badgeFor(cad::document::ObjectState state) {
    switch (state) {
        case cad::document::ObjectState::Failed:  return QStringLiteral("ERR");
        case cad::document::ObjectState::Blocked: return QStringLiteral("BLOCKED");
        case cad::document::ObjectState::NeedsPlugin: return QStringLiteral("PLUGIN");
        case cad::document::ObjectState::Dirty:   return QStringLiteral("•");
        case cad::document::ObjectState::Clean:   return {};
    }
    return {};
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

    ribbon_ = new proshell::Ribbon(top);
    topLayout->addWidget(ribbon_);

    // A hairline closing the ribbon off from the workspace below. Both Inventor and SolidWorks
    // separate the command area from the graphics area this way, and without it the ribbon and
    // the viewport read as one undifferentiated surface. QFrame::HLine is not used: it draws a
    // two-tone bevel that looks like a 1990s group box. This is one device pixel of the theme's
    // own line colour.
    auto* rule = new QWidget(top);
    rule->setFixedHeight(1);
    rule->setAutoFillBackground(true);
    {
        QPalette rulePalette = rule->palette();
        rulePalette.setColor(QPalette::Window, QColor(0xcf, 0xcd, 0xc9));
        rule->setPalette(rulePalette);
    }
    topLayout->addWidget(rule);

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
    fileMenu_->addAction(tr("Options..."), QKeySequence::Preferences, this,
                         [this] { showOptions(); });
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

QAction* MainWindow::parameterised(const char* id, const QString& label,
                                   const QString& iconName) {
    // Try beginCommand first; if the command has no parameters it returns false and we fall back to
    // invoking it directly, so a command gains a panel the day app/ gives it parameters with no
    // edit here. Same principle as commandOr.
    auto* real = command(id);
    if (real == nullptr) return planned(label, iconName);

    auto* action = new QAction(icon(iconName), label, this);
    action->setToolTip(real->toolTip());
    connect(action, &QAction::triggered, this, [this, id, real] {
        auto* c = controller();
        if (c != nullptr && c->beginCommand(id)) {
            syncCommandPanel();
        } else {
            real->trigger();
        }
    });
    // Enablement follows the real command's, so the two cannot disagree.
    action->setEnabled(real->isEnabled());
    parameterisedActions_.push_back({action, real});
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
    sketchConstraintActions_.clear();
    parameterisedActions_.clear();

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

    // In the sketch environment the ribbon collapses to ONE tab, as Inventor's does. The app
    // entered this mode for the user, so the commands follow without them choosing a mode.
    if (c->environment() == cad::app::Environment::Sketch) {
        auto* sketchTab = ribbon_->addTab(tr("Sketch"));
        auto* draw = sketchTab->addPanel(tr("Draw"));
        const auto addTool = [&](const QString& label, const QString& iconName,
                                 SketchCanvas::Tool tool, const QString& shortcut) {
            auto* action = new QAction(icon(iconName), label, this);
            action->setToolTip(tr("%1 (%2)").arg(label, shortcut));
            connect(action, &QAction::triggered, this, [this, tool] {
                const std::size_t index = session_.activeIndex();
                if (index < sketchCanvases_.size()) sketchCanvases_[index]->setTool(tool);
            });
            draw->addLarge(action);
        };
        addTool(tr("Select"), QStringLiteral("select"), SketchCanvas::Tool::Select,
                QStringLiteral("S"));
        addTool(tr("Line"), QStringLiteral("line"), SketchCanvas::Tool::Line, QStringLiteral("L"));
        addTool(tr("Circle"), QStringLiteral("circle"), SketchCanvas::Tool::Circle,
                QStringLiteral("C"));

        // Constrain. Enablement is derived from the SELECTION, so a button is live only when it
        // would actually apply -- the same rule as the model ribbon, and the reason none of these
        // can offer themselves before canvas selection existed.
        auto* constrain = sketchTab->addPanel(tr("Constrain"));
        const auto addConstraint = [&](const QString& label, const QString& iconName,
                                       cad::sketch::ConstraintKind kind, std::size_t needs,
                                       bool linesOnly) {
            auto* action = new QAction(icon(iconName), label, this);
            action->setToolTip(needs == 1 ? tr("%1 — select one curve").arg(label)
                                          : tr("%1 — select two curves").arg(label));
            connect(action, &QAction::triggered, this, [this, kind] {
                if (auto* ctl = controller()) ctl->applySketchConstraint(kind);
                refreshStatus();
            });
            sketchConstraintActions_.push_back({action, needs, linesOnly});
            constrain->addSmall(action);
        };
        using CK = cad::sketch::ConstraintKind;
        addConstraint(tr("Horizontal"), QStringLiteral("horizontal"), CK::Horizontal, 1, true);
        addConstraint(tr("Vertical"), QStringLiteral("vertical"), CK::Vertical, 1, true);
        addConstraint(tr("Parallel"), QStringLiteral("parallel"), CK::Parallel, 2, true);
        addConstraint(tr("Perpendicular"), QStringLiteral("perpendicular"), CK::Perpendicular, 2,
                      true);
        addConstraint(tr("Equal"), QStringLiteral("equal"), CK::EqualLength, 2, true);
        {
            // Radius carries a VALUE, so it asks. A dialog is the honest stopgap until the command
            // property panel exists (DESKTOP_UX 3.2), which is where a value belongs.
            auto* action = new QAction(icon(QStringLiteral("radius")), tr("Radius"), this);
            action->setToolTip(tr("Radius — select one circle or arc"));
            connect(action, &QAction::triggered, this, [this] {
                auto* ctl = controller();
                if (ctl == nullptr) return;
                bool ok = false;
                const double r = QInputDialog::getDouble(this, tr("Radius"), tr("Radius (mm):"),
                                                         10.0, 0.001, 1e6, 3, &ok);
                if (ok) ctl->applySketchRadius(r);
                refreshStatus();
            });
            sketchConstraintActions_.push_back({action, 1, false});
            constrain->addSmall(action);
        }

        // Delete is on the Sketch tab too: with selection working it is the most-used edit here,
        // and reaching for the model tab's Delete would leave the environment.
        auto* modify = sketchTab->addPanel(tr("Modify"));
        {
            auto* del = new QAction(icon(QStringLiteral("delete")), tr("Delete"), this);
            del->setToolTip(tr("Delete the selected sketch geometry (Del)"));
            connect(del, &QAction::triggered, this, [this] {
                if (auto* ctl = controller()) ctl->deleteSketchSelection();
                refreshStatus();
            });
            modify->addSmall(del);
        }

        auto* finish = sketchTab->addPanel(tr("Exit"));
        finish->addLarge(commandOr("sketch.finish", tr("Finish\nSketch"),
                                   QStringLiteral("sketch-finish")));
        finish->addSmall(commandOr("sketch.cancel", tr("Cancel"), QStringLiteral("delete")));

        ribbon_->setCurrentTab(0);
        refreshCommandStates();
        return;
    }

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
    create->addLarge(parameterised("feature.extrude", tr("Extrude"), QStringLiteral("extrude")));
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
    primitives->addLarge(parameterised("feature.box", tr("Box"), QStringLiteral("box")));
    primitives->addLarge(
        parameterised("feature.cylinder", tr("Cylinder"), QStringLiteral("cylinder")));

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
    sketchManage->addLarge(commandOr("sketch.edit", tr("Edit\nSketch"),
                                     QStringLiteral("sketch-edit")));
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
        
        auto* canvas = sketchCanvases_[docIndex];
        sketchCanvases_.erase(sketchCanvases_.begin() + static_cast<std::ptrdiff_t>(docIndex));
        workspaces_->removeWidget(canvas);
        canvas->deleteLater();

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

    auto* editor = new Viewport(*c, workspaces_);
    // Bring the GPU up on the first viewport. Idempotent -- bgfx is a process singleton, so the
    // second document shares the backend -- and a failure is reported rather than fatal: the
    // wireframe fallback keeps the shell fully usable on a machine with no usable device.
    if (!editor->attachRenderer()) {
        statusBar()->showMessage(tr("Viewport: %1").arg(editor->rendererError()));
    }
    // The sketch surface is a SIBLING in the stack, not an overlay on the viewport. A sketch is
    // edited face-on in its own 2D coordinate system; sharing the 3D camera would mean unprojecting
    // every click onto a plane before it meant anything.
    auto* canvas = new SketchCanvas(*c, workspaces_);
    workspaces_->addWidget(canvas);
    sketchCanvases_.push_back(canvas);
    connect(canvas, &SketchCanvas::sketchChanged, this, [this] { refreshStatus(); });
    workspaces_->addWidget(editor);
    editors_.push_back(editor);

    // Each document's controller drives the shared panels, but only while it is the active one —
    // otherwise a background recompute would repaint another document's tree.
    c->onDocumentChanged([this, c] {
        if (controller() != c) return;
        refreshTree();
        refreshProperties();
        refreshCommandStates();
        refreshSketchConstraintStates();
        syncCommandPanel();
        refreshStatus();
        // The title's dirty marker is derived from the document, so it has to be refreshed on
        // every edit — not only when the active document changes.
        syncTitle();
        // An ENVIRONMENT change has to re-derive both the workspace widget and the ribbon, and
        // neither happens on an ordinary document notification. Entering a sketch would otherwise
        // leave the 3D viewport on screen with the model ribbon above it -- the mode would exist in
        // app/ and be invisible in the shell. Guarded so an ordinary edit does not rebuild the
        // ribbon on every keystroke.
        if (c->environment() != lastEnvironment_) {
            lastEnvironment_ = c->environment();
            syncWorkspace();
            rebuildRibbon();
        }
        if (auto* w = workspaces_->currentWidget()) w->update();
    });
    c->onViewChanged([this, c] {
        if (controller() != c) return;
        if (auto* w = workspaces_->currentWidget()) {
            // markDirty, not update: the viewport caches its rendered frame and a plain repaint
            // deliberately reuses it. This is the signal that the scene itself moved.
            if (auto* view = qobject_cast<Viewport*>(w)) {
                view->markDirty();
            } else {
                w->update();
            }
        }
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
    if (index >= editors_.size()) return;
    auto* c = controller();
    const bool sketching = c != nullptr && c->environment() == cad::app::Environment::Sketch;
    if (sketching && index < sketchCanvases_.size()) {
        workspaces_->setCurrentWidget(sketchCanvases_[index]);
        // Frame the sketch on entry: one loaded from a file can be anywhere, and an empty canvas
        // showing the wrong region reads as "the sketch is gone".
        sketchCanvases_[index]->fit();
        sketchCanvases_[index]->setFocus();
    } else {
        workspaces_->setCurrentWidget(editors_[index]);
    }
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

QWidget* MainWindow::buildCommandPanel() {
    auto* panel = new QWidget(this);
    panel->setObjectName(QStringLiteral("commandPanel"));
    auto* column = new QVBoxLayout(panel);
    column->setContentsMargins(10, 10, 10, 10);
    column->setSpacing(8);

    commandTitle_ = new QLabel(panel);
    commandTitle_->setObjectName(QStringLiteral("commandTitle"));
    column->addWidget(commandTitle_);

    // OK and Cancel at the TOP, which is where SolidWorks puts them. Unfamiliar next to a desktop
    // dialog's bottom-right buttons, but this is a panel the user's eye enters from the top, and it
    // is the convention the reference application established.
    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(6);
    auto* ok = new QToolButton(panel);
    ok->setText(tr("OK"));
    ok->setObjectName(QStringLiteral("commandOk"));
    connect(ok, &QToolButton::clicked, this, [this] {
        if (auto* c = controller()) c->commitCommand();
        syncCommandPanel();
    });
    auto* cancel = new QToolButton(panel);
    cancel->setText(tr("Cancel"));
    cancel->setObjectName(QStringLiteral("commandCancel"));
    connect(cancel, &QToolButton::clicked, this, [this] {
        if (auto* c = controller()) c->cancelCommand();
        syncCommandPanel();
    });
    buttons->addWidget(ok);
    buttons->addWidget(cancel);
    buttons->addStretch(1);
    column->addLayout(buttons);

    commandFields_ = new QFormLayout;
    commandFields_->setContentsMargins(0, 6, 0, 0);
    commandFields_->setSpacing(6);
    column->addLayout(commandFields_);
    column->addStretch(1);
    return panel;
}

void MainWindow::syncCommandPanel() {
    auto* c = controller();
    const bool running = c != nullptr && !c->activeCommand().empty();

    if (!running) {
        leftStack_->setCurrentIndex(0);
        browserDock_->setWindowTitle(tr("Model"));
        return;
    }

    // Rebuilt each time rather than diffed: a command has a handful of fields, and reusing widgets
    // across different commands is how a stale editor from the last command ends up bound to this
    // one's parameter.
    while (commandFields_->rowCount() > 0) commandFields_->removeRow(0);

    for (const auto& p : c->commandParameters()) {
        auto* edit = new QLineEdit(QString::fromStdString(p.value), commandPanel_);
        const std::string name = p.name;
        // editingFinished, not textChanged: parsing every keystroke rejects "2 i" on the way to
        // "2 in" and fights the user as they type.
        connect(edit, &QLineEdit::editingFinished, this, [this, name, edit] {
            auto* ctl = controller();
            if (ctl == nullptr) return;
            if (!ctl->setCommandParameter(name, edit->text().toStdString())) {
                // Rejected: put the accepted value back, so the field never shows something the
                // model did not take.
                for (const auto& q : ctl->commandParameters()) {
                    if (q.name == name) edit->setText(QString::fromStdString(q.value));
                }
            }
            refreshStatus();
        });
        commandFields_->addRow(QString::fromStdString(p.label), edit);
    }

    commandTitle_->setText(QString::fromStdString(c->activeCommand()));
    browserDock_->setWindowTitle(tr("Properties"));
    leftStack_->setCurrentIndex(1);

    // Focus the first field so a command can be driven from the keyboard without reaching for the
    // mouse, which is how anyone fast actually works.
    if (commandFields_->rowCount() > 0) {
        if (auto* first = commandFields_->itemAt(0, QFormLayout::FieldRole)) {
            if (auto* w = first->widget()) w->setFocus();
        }
    }
}

void MainWindow::buildDocks() {
    browserDock_ = new QDockWidget(tr("Model"), this);
    auto* browserDock = browserDock_;
    browserDock->setFeatures(QDockWidget::DockWidgetMovable);
    browser_ = new QTreeWidget(browserDock);
    browser_->setHeaderHidden(true);
    browser_->setColumnCount(2);
    browser_->header()->setStretchLastSection(false);
    browser_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    browser_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    browser_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    browser_->setIndentation(14);

    // The left dock holds EITHER the tree or the running command, never both.
    //
    // SolidWorks' PropertyManager takes over this space rather than floating over the model
    // (UI_RESEARCH.md), and DESKTOP_UX 3.2 was corrected to match. A stack rather than a splitter:
    // the two are alternatives, and showing a squeezed tree beside a squeezed command panel gives
    // the worst of both.
    leftStack_ = new QStackedWidget(browserDock);
    leftStack_->addWidget(browser_);          // index 0
    commandPanel_ = buildCommandPanel();
    leftStack_->addWidget(commandPanel_);     // index 1
    browserDock->setWidget(leftStack_);
    addDockWidget(Qt::LeftDockWidgetArea, browserDock);
    resizeDocks({browserDock}, {290}, Qt::Horizontal);

    browser_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(browser_, &QTreeWidget::customContextMenuRequested, this,
            [this](const QPoint& at) { showBrowserMenu(at); });

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

void MainWindow::showOptions() {
    auto* c = controller();
    if (c == nullptr) {
        statusMessage_->setText(tr("Open a document to change options"));
        return;
    }
    const auto current = c->preferences();

    QDialog dialog(this);
    dialog.setWindowTitle(tr("Options"));
    auto* form = new QFormLayout(&dialog);

    auto* units = new QComboBox(&dialog);
    // Order matches units::UnitSystem so the index IS the enum value. Kept adjacent to the enum
    // rather than mapped, because a mapping table is one more thing to forget to update.
    units->addItems({tr("Millimetres"), tr("Centimetres"), tr("Metres"), tr("Inches"), tr("Feet")});
    units->setCurrentIndex(static_cast<int>(current.displayUnits));
    form->addRow(tr("Display units"), units);

    auto* navigation = new QComboBox(&dialog);
    navigation->addItems({tr("CAD (middle drag orbits)"), tr("Fusion (middle drag pans)"),
                          tr("Blender")});
    navigation->setCurrentIndex(static_cast<int>(current.navigation));
    navigation->setToolTip(tr("Which mouse button orbits. Match whichever application you came "
                              "from — this is muscle memory, not preference."));
    form->addRow(tr("Navigation"), navigation);

    auto* snap = new QDoubleSpinBox(&dialog);
    snap->setRange(0.0001, 100.0);
    snap->setDecimals(4);
    snap->setValue(current.snapTolerance);
    snap->setToolTip(tr("Endpoints closer than this are treated as one point when inferring "
                        "constraints from an imported file."));
    form->addRow(tr("Snap tolerance"), snap);

    auto* angle = new QDoubleSpinBox(&dialog);
    angle->setRange(0.0, 45.0);
    angle->setDecimals(2);
    angle->setValue(current.angleTolerance);
    angle->setSuffix(tr("°"));
    angle->setToolTip(tr("A line within this of an axis is inferred horizontal or vertical. Keep "
                         "it small: a 2° taper is design intent, not a drafting error."));
    form->addRow(tr("Angle tolerance"), angle);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    form->addRow(buttons);

    if (dialog.exec() != QDialog::Accepted) return;

    cad::app::Preferences next = current;
    next.displayUnits = static_cast<cad::units::UnitSystem>(units->currentIndex());
    next.navigation = static_cast<cad::render::NavigationPreset>(navigation->currentIndex());
    next.snapTolerance = snap->value();
    next.angleTolerance = angle->value();

    // Applied to EVERY open document, not just the active one. Units are a property of the user,
    // not of a file, and having the tab you switch to still show millimetres would read as a bug.
    for (std::size_t i = 0; i < session_.count(); ++i) {
        session_.activate(i);
        if (auto* ctl = session_.active()) ctl->setPreferences(next);
    }
    refreshProperties();
    refreshStatus();
    statusMessage_->setText(tr("Options applied"));
}

void MainWindow::showBrowserMenu(const QPoint& at) {
    auto* c = controller();
    QTreeWidgetItem* item = browser_->itemAt(at);
    if (c == nullptr || item == nullptr) return;
    const QVariant raw = item->data(0, Qt::UserRole);
    if (!raw.isValid()) return;   // the document root, which has no feature actions
    const cad::document::ObjectId id{raw.toULongLong()};

    const auto object = c->tree();
    const auto found = std::find_if(object.begin(), object.end(),
                                    [&](const auto& t) { return t.id == id; });
    if (found == object.end()) return;

    QMenu menu(this);
    // Edit Sketch first and only for sketches: the most common reason to right-click one, and
    // offering it on a box would be a menu entry that exists to be refused.
    if (found->type == "Sketch") {
        menu.addAction(icon(QStringLiteral("sketch-edit"), 16), tr("Edit Sketch"), this,
                       [this, id] { if (auto* ctl = controller()) ctl->editSketch(id); });
        menu.addSeparator();
    }
    menu.addAction(tr("Rename..."), this, [this, id, found] {
        bool ok = false;
        const QString name = QInputDialog::getText(this, tr("Rename"), tr("Name:"),
                                                   QLineEdit::Normal,
                                                   QString::fromStdString(found->label), &ok);
        if (ok && !name.trimmed().isEmpty()) {
            if (auto* ctl = controller()) ctl->rename(id, name.trimmed().toStdString());
        }
    });
    menu.addAction(icon(QStringLiteral("rollback"), 16), tr("Roll Back to Here"), this,
                   [this, id] { if (auto* ctl = controller()) ctl->setRollback(id); });
    if (c->rollback().has_value()) {
        menu.addAction(icon(QStringLiteral("rollforward"), 16), tr("Roll Forward"), this,
                       [this] { if (auto* ctl = controller()) ctl->setRollback(std::nullopt); });
    }
    menu.addSeparator();
    menu.addAction(icon(QStringLiteral("delete"), 16), tr("Delete"), this,
                   [this, id] { if (auto* ctl = controller()) ctl->remove(id); });

    menu.exec(browser_->viewport()->mapToGlobal(at));
}

void MainWindow::refreshTree() {
    updatingUi_ = true;
    browser_->clear();

    auto* c = controller();
    // Which features the rollback marker suspends. Collected once rather than asked per row.
    std::set<std::uint64_t> rolledBack;
    if (c != nullptr) {
        for (const auto& item : c->tree()) {
            if (c->isRolledBack(item.id)) rolledBack.insert(item.id.value);
        }
    }
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

            // A badge in the second column, not a coloured name alone. Colour is not enough on its
            // own: it is invisible to a colour-blind user and disappears entirely in a screenshot
            // pasted into a bug report, which is exactly when the state matters most.
            const QString badge = badgeFor(item.state);
            if (!badge.isEmpty()) {
                node->setText(1, badge);
                node->setForeground(1, stateColour(item.state));
                node->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
            }
            // Suspended by the rollback marker: struck through, as Inventor greys everything below
            // its End-of-Part marker. Distinct from failed, because nothing is wrong.
            if (rolledBack.count(item.id.value) != 0) {
                QFont font = node->font(0);
                font.setStrikeOut(true);
                node->setFont(0, font);
                node->setForeground(0, QColor(0xa8, 0xab, 0xaf));
            }
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

void MainWindow::refreshSketchConstraintStates() {
    auto* c = controller();
    const std::size_t selected = c != nullptr ? c->sketchSelection().size() : 0;
    for (const auto& entry : sketchConstraintActions_) {
        bool enabled = selected == entry.needs;
        // Type is checked here as well as in the Controller, so the button greys out rather than
        // enabling and then refusing -- the rule the model ribbon already follows.
        if (enabled && entry.linesOnly && c != nullptr) {
            for (const auto id : c->sketchSelection()) {
                const auto* g = c->activeSketch() ? c->activeSketch()->find(id) : nullptr;
                if (g == nullptr || g->kind != cad::sketch::GeoKind::Line) enabled = false;
            }
        }
        entry.action->setEnabled(enabled);
    }
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
    // In a sketch the stats that matter are the sketch's, not the model's. Degrees of freedom are
    // the number that tells a user whether the sketch is finished, and it is the reason the solver
    // reports it at all -- leaving it in a struct nobody displays wastes the one signal a sketcher
    // gives you about your own work.
    if (c->environment() == cad::app::Environment::Sketch) {
        const auto& report = c->lastSketchSolve();
        const auto* sketch = c->activeSketch();
        const std::size_t curves = sketch != nullptr ? sketch->geometry().size() : 0;
        const std::size_t constraints = sketch != nullptr ? sketch->constraints().size() : 0;
        QString text = tr("%1 curves · %2 constraints · ").arg(curves).arg(constraints);
        if (!report.conflicting.empty()) {
            text += tr("OVER-CONSTRAINED (%1)").arg(report.conflicting.size());
        } else if (report.dofs == 0) {
            text += tr("fully constrained");
        } else {
            text += tr("%1 degrees of freedom").arg(report.dofs);
        }
        if (!c->sketchSelection().empty()) {
            text += tr("  ·  %1 selected").arg(c->sketchSelection().size());
        }
        statusStats_->setText(text);
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

    // Frame cost, split into its two halves. Shown because "the viewport feels slow" is not a
    // number, and the two halves have entirely different fixes: submit is the scene, capture is
    // the price of the offscreen readback path.
    if (c->rendererAttached()) {
        const auto t = c->lastRenderTiming();
        const double total = t.submitMs + t.captureMs;
        if (total > 0.0) {
            text += tr("  ·  %1 ms/frame (submit %2, readback %3)")
                        .arg(total, 0, 'f', 1)
                        .arg(t.submitMs, 0, 'f', 1)
                        .arg(t.captureMs, 0, 'f', 1);
        }
    }
    statusStats_->setText(text);
}

}  // namespace cadqt
