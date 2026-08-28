#include "MainWindow.h"

#include "PluginManager.h"
#include "cad/abi/cad_plugin_abi.h"
#include "cad/log/Log.h"
#include "proshell/HomePage.h"
#include "Icons.h"
#include "proshell/Ribbon.h"
#include "proshell/Settings.h"
#include "proshell/Theme.h"
#include "proshell/SettingsModel.h"
#include "proshell/SettingsWindow.h"
#include "SketchCanvas.h"
#include "Viewport.h"

#include <QApplication>
#include <QTimer>
#include <cstdlib>
#include <filesystem>
#include <QCoreApplication>
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
    setProductName(tr("vCAD"));
    resize(1500, 940);

    // The frame first, then vCAD's contents into it. buildChrome() is not called by ShellWindow's
    // own constructor precisely so that everything below it can assume the widgets exist.
    buildChrome();
    buildQuickAccess();
    buildBrowserAndProperties();
    buildWorkspaces();
    buildStatusFields();

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

MainWindow::~MainWindow() {
    // LAST, and only here. Plugin descriptors -- every settings label and ribbon caption a plugin
    // contributed -- point into memory this session owns, so releasing it any earlier would leave
    // whatever is still showing them rendering freed strings. Nothing unloads the libraries
    // themselves; see Loader.h for why that is deliberate.
    if (pluginSession_ != 0) cad_session_release(pluginSession_);
}

// ── top area ────────────────────────────────────────────────────────────────────────────

void MainWindow::buildQuickAccess() {
    // vCAD's own buttons on the frame's strip. The strip, the File tab, the ribbon and the
    // hairline below it are ShellWindow's; what a New button DOES is ours.
    addQuickAccessButton(QStringLiteral("new"), tr("New part"),
                         [this] { createDocument(DocumentKind::Part); }, QKeySequence::New);
    addQuickAccessButton(QStringLiteral("open"), tr("Open"), [this] { openDocument(); },
                         QKeySequence::Open);
    addQuickAccessButton(QStringLiteral("save"), tr("Save"), [this] { saveDocument(false); },
                         QKeySequence::Save);
    addQuickAccessSpacing();

    // Orbit lives on the STRIP, not only in the View tab, because the strip is the one surface
    // visible in every environment. In the sketch environment the ribbon shows the Sketch tab, so
    // the View tab's copy is a tab-switch away at exactly the moment a user most wants to turn the
    // part around — which is indistinguishable from the application having no way to orbit.
    orbitButton_ = addQuickAccessButton(QStringLiteral("ortho"), tr("Orbit (O)"), [this] {
        if (auto* c = controller()) setOrbitMode(!c->orbitMode());
    });
    orbitButton_->setCheckable(true);

    addQuickAccessButton(QStringLiteral("undo"), tr("Undo"), [this] {
        if (auto* c = controller()) c->undo();
    }, QKeySequence::Undo);
    addQuickAccessButton(QStringLiteral("redo"), tr("Redo"), [this] {
        if (auto* c = controller()) c->redo();
    }, QKeySequence::Redo);

    // Selection filter. Non-negotiable in CAD: picking an edge inside a dense assembly is
    // otherwise impossible, and a filter that lives in a preferences dialog is a filter nobody
    // knows is on. It sits at the right of the strip, visible at all times, because "why did my
    // click select the whole body" is a question the UI should already be answering.
    //
    // On the strip but not OF it, which is why ShellWindow takes it as an opaque widget rather
    // than knowing what a selection filter is: an application with nothing to select has no use
    // for the concept, and one with a different notion of selection needs a different control.
    //
    // The shell owns the setting for now. It becomes a Controller concern the day IPicker
    // resolves a pixel to the nearest entity of a requested TYPE rather than to one element —
    // see DESKTOP_UX.md 3.3, which is a resolution rule over the ID buffer, not new GPU work.
    //
    // Hidden on Home, which has nothing to select. A filter offering to restrict picking on a
    // page with no geometry is chrome pretending to be a control.
    filterBar_ = new QWidget(this);
    auto* filterRow = new QHBoxLayout(filterBar_);
    filterRow->setContentsMargins(0, 0, 0, 0);
    filterRow->setSpacing(2);

    auto* filterLabel = new QLabel(tr("Select"), filterBar_);
    filterLabel->setObjectName(QStringLiteral("qatFilterLabel"));
    filterRow->addWidget(filterLabel);

    selectionFilter_ = new QButtonGroup(this);
    selectionFilter_->setExclusive(true);
    // Auto FIRST, and the default.
    //
    // A fixed level makes the user declare in advance what kind of thing they are about to point
    // at. Auto resolves to whatever was actually hit — the ranking under the pointer already knows
    // (docs/design/SELECTION.md) — and the explicit levels stay for the case that needs them: when
    // several kinds overlap and you want only one of them, which is what a filter is FOR.
    //
    // Same list as the iPad, which reaches the same rule through a double tap instead of a control.
    const std::array<std::pair<const char*, const char*>, 5> filters{{
        {"Auto", "Select whatever is under the pointer"},
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
    addQuickAccessWidget(filterBar_);
    // CONNECTED to the model, which it was not.
    //
    // This control existed, looked right, and did nothing: it re-rendered the status bar and never
    // told the Controller, so choosing "Edge" left clicks resolving to whole bodies. A filter that
    // does not filter is worse than an absent one, because the user believes it.
    connect(selectionFilter_, &QButtonGroup::idClicked, this, [this](int) {
        applySelectionFilter();
        refreshStatus();
    });

    // File menu, on the File tab rather than a menu bar.
    auto* menu = fileMenu();
    menu->addAction(icon(QStringLiteral("part"), 16), tr("New Part"), this,
                    [this] { createDocument(DocumentKind::Part); });
    for (const auto kind : {DocumentKind::Assembly, DocumentKind::Drawing,
                            DocumentKind::Presentation}) {
        auto* action = menu->addAction(
            icon(QString::fromUtf8(cad::app::toString(kind)).toLower(), 16),
            tr("New %1").arg(QString::fromUtf8(cad::app::toString(kind))));
        // Present but disabled: a user should see the app's intended shape, not wonder whether
        // assemblies exist.
        action->setEnabled(cad::app::implemented(kind));
    }
    menu->addSeparator();
    menu->addAction(icon(QStringLiteral("open"), 16), tr("Open..."), QKeySequence::Open, this,
                    [this] { openDocument(); });
    menu->addAction(icon(QStringLiteral("save"), 16), tr("Save"), QKeySequence::Save, this,
                    [this] { saveDocument(false); });
    menu->addAction(tr("Save As..."), QKeySequence::SaveAs, this, [this] { saveDocument(true); });
    menu->addSeparator();
    menu->addAction(icon(QStringLiteral("import"), 16), tr("Export..."),
                    QKeySequence(QStringLiteral("Ctrl+Shift+E")), this, [this] { exportDocument(); });
    menu->addSeparator();
    menu->addAction(tr("Home"), this, [this] { session_.activateHome(); });
    menu->addSeparator();
    menu->addAction(tr("Options..."), QKeySequence::Preferences, this, [this] { showOptions(); });
    menu->addSeparator();
    menu->addAction(tr("Exit"), QKeySequence::Quit, this, &QWidget::close);
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
        setStatusMessage(tr("Open failed"));
        return;
    }
    // Session::openDocument fires its changed callback, which rebuilds the tabs and the ribbon.
    setStatusMessage(tr("Opened %1").arg(QFileInfo(path).fileName()));
}

bool MainWindow::exportDocument() {
    auto* c = controller();
    if (session_.homeActive() || c == nullptr) {
        setStatusMessage(tr("There is no document to export"));
        return false;
    }

    // The filter is built from the REGISTRY, so a format compiled in conditionally appears exactly
    // when it is available rather than being promised in a dialog and refused on write.
    QStringList filters;
    for (const auto& format : cad::app::Controller::exportFormats()) {
        QStringList patterns;
        for (const std::string& extension : format.extensions) {
            patterns << QStringLiteral("*%1").arg(QString::fromStdString(extension));
        }
        // Mesh-only formats say so in the dialog. Exporting a solid model to STL and discovering
        // later that the B-rep is gone is a discovery that should happen before the file is
        // written, not when someone tries to open it in CAD.
        const QString name = format.solids
                                 ? QString::fromStdString(format.displayName)
                                 : tr("%1 — mesh only, no solid geometry")
                                       .arg(QString::fromStdString(format.displayName));
        filters << QStringLiteral("%1 (%2)").arg(name, patterns.join(QLatin1Char(' ')));
    }
    if (filters.isEmpty()) {
        setStatusMessage(tr("No export formats are available in this build"));
        return false;
    }

    const auto& document = session_.documents()[session_.activeIndex()];
    const QString suggested = QString::fromStdString(document.title);
    QString selected = filters.front();
    const QString path = QFileDialog::getSaveFileName(this, tr("Export"), suggested,
                                                      filters.join(QStringLiteral(";;")), &selected);
    if (path.isEmpty()) return false;

    if (!c->exportDocument(path.toStdString())) {
        // The Controller's own refusal, which reached the status bar through its status callback.
        // Repeating it in a dialog rather than inventing a second wording: there is one reason the
        // export failed and the user should see that one.
        QMessageBox::warning(this, tr("Could not export"), statusMessage(), QMessageBox::Ok);
        return false;
    }
    setStatusMessage(tr("Exported %1").arg(QFileInfo(path).fileName()));
    return true;
}

bool MainWindow::saveDocument(bool saveAs) {
    if (session_.homeActive() || session_.active() == nullptr) {
        setStatusMessage(tr("There is no document to save"));
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
        setStatusMessage(tr("Save failed"));
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

bool MainWindow::confirmClose() {
    // Every open document, not just the active one: quitting must not silently drop edits in a tab
    // the user is not looking at.
    for (std::size_t i = 0; i < session_.count(); ++i) {
        session_.activate(i);
        if (!confirmDiscardChanges()) return false;
    }
    return true;
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
        setStatusMessage(tr("Import failed"));
        return;
    }
    refreshTree();
    refreshCommandStates();
}

void MainWindow::rebuildRibbon() {
    // Tabs are a function of the active workspace. Home contributes none of its own.
    ribbon()->clearTabs();
    actions_.clear();
    sketchConstraintActions_.clear();
    parameterisedActions_.clear();

    auto* c = controller();
    if (c == nullptr) {
        // Home's ribbon, matching Inventor: application-level tabs only. Creating documents is
        // the Home page's own job — the sidebar's New... button — so the ribbon does not
        // duplicate it.
        auto* tools = ribbon()->addTab(tr("Tools"));
        auto* options = tools->addPanel(tr("Options"));
        options->addLarge(planned(tr("Application\nOptions"), QStringLiteral("parameters")));
        options->addLarge(planned(tr("Document\nSettings"), QStringLiteral("note")));
        auto* cachePanel = tools->addPanel(tr("Cache"));
        cachePanel->addLarge(planned(tr("Cache\nStatus"), QStringLiteral("cache")));
        cachePanel->addSmall(planned(tr("Purge Local"), QStringLiteral("purge")));

        // Plugins, and a real command rather than a `planned()` stand-in -- the loader exists and
        // the manager reads actual installed manifests. On Home rather than in a document's
        // ribbon because plugins are application-level: what is installed does not depend on what
        // is open, and a plugin registers its types at startup for every document at once.
        auto* pluginPanel = tools->addPanel(tr("Plugins"));
        {
            auto* manage = new QAction(icon(QStringLiteral("parameters")),
                                       tr("Manage\nPlugins"), this);
            manage->setToolTip(tr("See which plugins are installed and whether vCAD can use them"));
            connect(manage, &QAction::triggered, this, [this] { showPluginManager(); });
            pluginPanel->addLarge(manage);
        }

        auto* collaborate = ribbon()->addTab(tr("Collaborate"));
        auto* project = collaborate->addPanel(tr("Project"));
        project->addLarge(planned(tr("Projects"), QStringLiteral("assembly")));
        project->addSmall(planned(tr("Search Paths"), QStringLiteral("open")));

        ribbon()->setCurrentTab(0);
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
        auto* sketchTab = ribbon()->addTab(tr("Sketch"));
        auto* draw = sketchTab->addPanel(tr("Draw"));
        // The tool is set on the CONTROLLER, not on a canvas widget: the sketch is drawn in the
        // 3D viewport now, and the tool has to mean the same thing to any shell that hosts it.
        const auto addTool = [&](const QString& label, const QString& iconName,
                                 cad::app::Controller::SketchTool tool, const QString& shortcut) {
            auto* action = new QAction(icon(iconName), label, this);
            action->setToolTip(tr("%1 (%2)").arg(label, shortcut));
            action->setCheckable(true);
            action->setShortcut(QKeySequence(shortcut));
            connect(action, &QAction::triggered, this, [this, tool] {
                if (auto* c = controller()) c->setSketchTool(tool);
                refreshSketchToolStates();
            });
            sketchToolActions_.push_back({action, tool});
            draw->addLarge(action);
        };
        addTool(tr("Select"), QStringLiteral("select"),
                cad::app::Controller::SketchTool::Select, QStringLiteral("S"));
        addTool(tr("Line"), QStringLiteral("line"),
                cad::app::Controller::SketchTool::Line, QStringLiteral("L"));
        addTool(tr("Circle"), QStringLiteral("circle"),
                cad::app::Controller::SketchTool::Circle, QStringLiteral("C"));
        // R for rectangle, as every CAD application binds it. Two clicks for opposite corners, or
        // one drag — the same tool either way.
        addTool(tr("Rectangle"), QStringLiteral("rectangle"),
                cad::app::Controller::SketchTool::Rectangle, QStringLiteral("R"));

        // Trim sits with the drawing tools because it is used in the same breath as them — draw
        // past a corner, press T, click the overhang — and because it is modal in exactly the same
        // way. Inventor and SolidWorks both bind T.
        addTool(tr("Trim"), QStringLiteral("trim"),
                cad::app::Controller::SketchTool::Trim, QStringLiteral("T"));

        // Dimension is bound to D, as Inventor and SolidWorks both do. Click a curve and type: the
        // dimension is created at the size the curve already is, and the number replaces it.
        addTool(tr("Dimension"), QStringLiteral("dimension"),
                cad::app::Controller::SketchTool::Dimension, QStringLiteral("D"));

        // Look At: re-aim at the sketch plane. Entering a sketch already does this, but a user
        // orbits away to see the part in context and then wants back — without it the only way
        // back is to leave the sketch and re-enter it.
        auto* view = sketchTab->addPanel(tr("View"));
        auto* lookAt = new QAction(icon(QStringLiteral("ortho")), tr("Look At"), this);
        lookAt->setShortcut(QKeySequence(QStringLiteral("F")));
        lookAt->setToolTip(tr("Look At (F) — face the sketch plane"));
        connect(lookAt, &QAction::triggered, this, [this] {
            if (auto* c = controller()) {
                c->alignCameraToSketch();
                setStatusMessage(tr("Facing the sketch plane"));
            }
        });
        view->addLarge(lookAt);
        sketchViewActions_.push_back(lookAt);

        // Slice: cut away what is between the viewer and the sketch plane. Checkable, because it
        // is a mode you stay in while drawing on a buried face.
        auto* slice = new QAction(icon(QStringLiteral("section")), tr("Slice"), this);
        slice->setCheckable(true);
        slice->setToolTip(tr("Slice — hide material in front of the sketch plane"));
        connect(slice, &QAction::triggered, this, [this](bool on) {
            if (auto* c = controller()) {
                c->setSliceEnabled(on);
                setStatusMessage(on ? tr("Slice on — material in front of the sketch is hidden")
                                    : tr("Slice off"));
            }
        });
        view->addLarge(slice);
        sketchViewActions_.push_back(slice);
        sliceAction_ = slice;

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

        ribbon()->setCurrentTab(0);
        refreshCommandStates();
        return;
    }

    // ── 3D Model ────────────────────────────────────────────────────────────────────────
    auto* model = ribbon()->addTab(tr("3D Model"));

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
    // `parameterised`, not `planned`: Revolve takes an angle, and defaults to a full turn — the same
    // default computeRevolve uses when none is stored, so the panel and the compute agree.
    create->addLarge(parameterised("feature.revolve", tr("Revolve"), QStringLiteral("revolve")));
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
    // `parameterised`, not `planned`: Hole takes a diameter and a depth, so it opens the command
    // panel the way Box and Cylinder do rather than drilling a size nobody chose.
    modify->addLarge(parameterised("feature.hole", tr("Hole"), QStringLiteral("hole")));
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
    // Move sits beside Direct because both edit a body's placement rather than its shape. Direct
    // (push-pull on a face) is still a stand-in; Move is real and takes a vector.
    modify->addSmall(parameterised("feature.translate", tr("Move"), QStringLiteral("move")));
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
    auto* sketch = ribbon()->addTab(tr("Sketch"));
    auto* sketchManage = sketch->addPanel(tr("Manage"));
    sketchManage->addLarge(commandOr("feature.sketch", tr("Start\nSketch"),
                                     QStringLiteral("sketch")));
    sketchManage->addLarge(commandOr("sketch.edit", tr("Edit\nSketch"),
                                     QStringLiteral("sketch-edit")));
    sketchManage->addLarge(planned(tr("Delete\nSketch"), QStringLiteral("delete")));

    // ── Inspect ─────────────────────────────────────────────────────────────────────────
    auto* inspect = ribbon()->addTab(tr("Inspect"));
    auto* measure = inspect->addPanel(tr("Measure"));
    measure->addLarge(planned(tr("Measure"), QStringLiteral("measure")));
    auto* analysis = inspect->addPanel(tr("Analysis"));
    analysis->addLarge(planned(tr("Section\nView"), QStringLiteral("section")));
    analysis->addSmall(planned(tr("Mass Properties"), QStringLiteral("mass")));
    analysis->addSmall(planned(tr("Draft Analysis"), QStringLiteral("draft")));

    // ── Annotate ────────────────────────────────────────────────────────────────────────
    auto* annotate = ribbon()->addTab(tr("Annotate"));
    auto* annotation = annotate->addPanel(tr("3D Annotation"));
    annotation->addLarge(planned(tr("Dimension"), QStringLiteral("dimension")));
    annotation->addLarge(planned(tr("Note"), QStringLiteral("note")));

    // ── Manage ──────────────────────────────────────────────────────────────────────────
    auto* manage = ribbon()->addTab(tr("Manage"));
    auto* parameters = manage->addPanel(tr("Parameters"));
    parameters->addLarge(planned(tr("Parameters"), QStringLiteral("parameters")));
    // The DDC, surfaced in the UI. No other CAD application has this panel because no other CAD
    // application has a content-addressed recompute cache (ADR 0004).
    auto* cache = manage->addPanel(tr("Cache"));
    cache->addLarge(planned(tr("Cache\nStatus"), QStringLiteral("cache")));
    cache->addSmall(planned(tr("Purge Local"), QStringLiteral("purge")));

    // ── View ────────────────────────────────────────────────────────────────────────────
    auto* view = ribbon()->addTab(tr("View"));
    auto* navigate = view->addPanel(tr("Navigate"));

    // A VISIBLE way to rotate the view. Orbit is otherwise on the middle button or on Alt, and
    // neither is discoverable: a laptop has no middle button, and nobody finds a modifier chord by
    // looking at the screen. Checkable, because it is a mode the user stays in until they leave it.
    orbitAction_ = new QAction(icon(QStringLiteral("ortho")), tr("Orbit"), this);
    orbitAction_->setCheckable(true);
    orbitAction_->setShortcut(QKeySequence(QStringLiteral("O")));
    orbitAction_->setToolTip(tr("Orbit (O) — drag to rotate. Alt-drag orbits at any time."));
    connect(orbitAction_, &QAction::triggered, this, [this](bool on) { setOrbitMode(on); });
    navigate->addLarge(orbitAction_);

    navigate->addLarge(commandOr("view.fit", tr("Fit"), QStringLiteral("fit")));
    navigate->addLarge(commandOr("view.ortho", tr("Ortho"), QStringLiteral("ortho")));
    auto* appearance = view->addPanel(tr("Appearance"));
    appearance->addSmall(planned(tr("Shaded"), QStringLiteral("shaded")));
    appearance->addSmall(planned(tr("Shaded + Edges"), QStringLiteral("shaded-edges")));
    appearance->addSmall(planned(tr("Wireframe"), QStringLiteral("wireframe")));
    auto* visibility = view->addPanel(tr("Visibility"));
    visibility->addSmall(planned(tr("Origin Planes"), QStringLiteral("origin")));
    visibility->addSmall(planned(tr("All Sketches"), QStringLiteral("sketches")));

    ribbon()->setCurrentTab(0);
    refreshCommandStates();
}

// ── workspaces ──────────────────────────────────────────────────────────────────────────

void MainWindow::buildWorkspaces() {
    // The splitter, the page stack and the bottom tab bar are the frame's. What goes IN them, and
    // what a tab means, is vCAD's -- see the note on ShellWindow for why documents did not move.
    home_ = new proshell::HomePage(homeModel_, workspaces());
    workspaces()->addWidget(home_);

    // Home's rail is built by HomePage but placed by the frame, so it spans the full window height
    // past the document tab bar rather than being clipped by it.
    setSidebar(home_->sidebar(), proshell::HomePage::sidebarDefaultWidth());

    connect(home_, &proshell::HomePage::openBrowseRequested, this, [this] { openDocument(); });
    connect(home_, &proshell::HomePage::openRequested, this,
            [this](const QString& path) { openPath(path); });
    connect(home_, &proshell::HomePage::createRequested, this,
            [this](int kind) { createDocument(static_cast<DocumentKind>(kind)); });

    connect(documentTabs(), &QTabBar::currentChanged, this, [this](int index) {
        if (updatingUi_ || index < 0) return;
        // Tab 0 is Home, which is not a document — hence index-1 into the document list.
        if (index == 0) {
            session_.activateHome();
        } else {
            session_.activate(static_cast<std::size_t>(index - 1));
        }
    });
    connect(documentTabs(), &QTabBar::tabCloseRequested, this, [this](int index) {
        if (index <= 0) return;   // Home cannot be closed
        const auto docIndex = static_cast<std::size_t>(index - 1);
        auto* editor = editors_[docIndex];
        editors_.erase(editors_.begin() + static_cast<std::ptrdiff_t>(docIndex));
        workspaces()->removeWidget(editor);
        editor->deleteLater();

        auto* canvas = sketchCanvases_[docIndex];
        sketchCanvases_.erase(sketchCanvases_.begin() + static_cast<std::ptrdiff_t>(docIndex));
        workspaces()->removeWidget(canvas);
        canvas->deleteLater();

        session_.close(docIndex);
    });
}

void MainWindow::selectRibbonTab(int index) {
    if (ribbon() != nullptr) ribbon()->setCurrentTab(index);
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

void MainWindow::selectBrowserRowForShot(int row) {
    // The Nth FEATURE, which is a child of the document root -- not the Nth top-level row. Row 0 at
    // top level is "Part1", whose id is 0 because it is the document rather than a feature, so
    // selecting it correctly selects nothing and looks exactly like selection being broken.
    if (browser_ == nullptr || browser_->topLevelItemCount() == 0) return;
    auto* root = browser_->topLevelItem(0);
    if (root == nullptr || row < 0 || row >= root->childCount()) return;
    browser_->setCurrentItem(root->child(row));
}

Viewport* MainWindow::probeViewport() noexcept {
    // The active document's editor, found the same way the rest of the window finds it.
    const auto index = session_.activeIndex();
    if (index >= session_.documents().size()) return nullptr;
    // editors_ holds QWidget*, because a document's editor is not always a 3D viewport — a drawing
    // or a presentation will have its own. A failed cast is the honest answer for those.
    return index < editors_.size() ? qobject_cast<Viewport*>(editors_[index]) : nullptr;
}

void MainWindow::applySelectionFilter() {
    // The control and the model, kept in step.
    //
    // They were not: this filter bar re-rendered the status text and never told the Controller, so
    // choosing "Edge" left every click resolving to a whole body. A filter that does not filter is
    // worse than an absent one, because the user believes it.
    //
    // Applied through the ACTIVE document rather than stored once, because each document has its
    // own Controller and a new one would otherwise start at the default while the button still
    // showed the last choice.
    auto* c = controller();
    if (c == nullptr || selectionFilter_ == nullptr) return;
    using Level = cad::app::Controller::SelectionLevel;
    static constexpr std::array<Level, 5> kLevels{Level::Auto, Level::Body, Level::Face,
                                                  Level::Edge, Level::Vertex};
    const int id = selectionFilter_->checkedId();
    if (id >= 0 && id < static_cast<int>(kLevels.size())) {
        c->setSelectionLevel(kLevels[static_cast<std::size_t>(id)]);
    }
}

void MainWindow::createDocument(DocumentKind kind) {
    if (!cad::app::implemented(kind)) {
        setStatusMessage(
            tr("%1 documents are not implemented yet").arg(QString::fromUtf8(
                cad::app::toString(kind))));
        return;
    }
    const std::size_t index = session_.create(kind);
    auto* c = session_.documents()[index].controller.get();

    // A new document inherits the filter the user is looking at, rather than silently reverting to
    // the default while the control still shows their choice.
    using Level = cad::app::Controller::SelectionLevel;
    static constexpr std::array<Level, 5> kLevels{Level::Auto, Level::Body, Level::Face,
                                                  Level::Edge, Level::Vertex};
    if (selectionFilter_ != nullptr) {
        const int id = selectionFilter_->checkedId();
        if (id >= 0 && id < static_cast<int>(kLevels.size())) {
            c->setSelectionLevel(kLevels[static_cast<std::size_t>(id)]);
        }
    }

    auto* editor = new Viewport(*c, workspaces());
    // Bring the GPU up on the first viewport. Idempotent -- bgfx is a process singleton, so the
    // second document shares the backend -- and a failure is reported rather than fatal: the
    // wireframe fallback keeps the shell fully usable on a machine with no usable device.
    if (!editor->attachRenderer()) {
        statusBar()->showMessage(tr("Viewport: %1").arg(editor->rendererError()));
    }

    // A click that selects nothing has a REASON — a curved face, geometry the naming layer never
    // named, empty space — and the status bar is where a user looks for it. Swallowing it is how a
    // pick that is working correctly looks like one that is broken.
    connect(editor, &Viewport::pickMessage, this, [this](const QString& text) {
        setStatusMessage(text);
    });
    connect(editor, &Viewport::dimensionChanged, this, [this] { refreshStatus(); });
    // The sketch surface is a SIBLING in the stack, not an overlay on the viewport. A sketch is
    // edited face-on in its own 2D coordinate system; sharing the 3D camera would mean unprojecting
    // every click onto a plane before it meant anything.
    auto* canvas = new SketchCanvas(*c, workspaces());
    workspaces()->addWidget(canvas);
    sketchCanvases_.push_back(canvas);
    connect(canvas, &SketchCanvas::sketchChanged, this, [this] { refreshStatus(); });
    workspaces()->addWidget(editor);
    editors_.push_back(editor);

    // Each document's controller drives the shared panels, but only while it is the active one —
    // otherwise a background recompute would repaint another document's tree.
    c->onDocumentChanged([this, c] {
        if (controller() != c) return;
        refreshTree();
        refreshProperties();
        refreshCommandStates();
        refreshSketchConstraintStates();
        refreshSketchToolStates();
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
        if (auto* w = workspaces()->currentWidget()) w->update();
    });
    c->onViewChanged([this, c] {
        if (controller() != c) return;
        if (auto* w = workspaces()->currentWidget()) {
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
        setStatusMessage(QString::fromStdString(text));
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
    leftDock()->setVisible(!home);
    rightDock()->setVisible(!home);
    if (filterBar_ != nullptr) filterBar_->setVisible(!home);
    setSidebarVisible(home);

    if (home) {
        workspaces()->setCurrentWidget(home_);
        home_->refresh();
        setWindowTitle(tr("vCAD"));
        return;
    }
    const std::size_t index = session_.activeIndex();
    if (index >= editors_.size()) return;
    auto* c = controller();
    // The VIEWPORT, always. Sketching used to swap to a separate 2D canvas, which put the user in
    // a different world from their model: the part vanished, the camera was unrelated to the one
    // they had arranged, and finishing the sketch teleported them back. A sketch is drawn on a
    // plane in the same space as the model, so it is edited there — the camera moves to the plane
    // (Controller::alignCameraToSketch) and the geometry is drawn as an overlay while it is being
    // made (Controller::pushSketchOverlay).
    workspaces()->setCurrentWidget(editors_[index]);
    editors_[index]->setFocus();
    if (index < session_.count()) {
        setWindowTitle(tr("vCAD — %1")
                           .arg(QString::fromStdString(session_.documents()[index].title)));
    }
}

void MainWindow::refreshDocumentTabs() {
    updatingUi_ = true;
    while (documentTabs()->count() > 0) documentTabs()->removeTab(0);

    documentTabs()->addTab(tr("Home"));
    for (const auto& doc : session_.documents()) {
        const int i = documentTabs()->addTab(QString::fromStdString(doc.title));
        documentTabs()->setTabIcon(
            i, icon(QString::fromUtf8(cad::app::toString(doc.kind)).toLower(), 14));
    }
    documentTabs()->setTabsClosable(true);
    // Home has no close button, because it cannot be closed (ADR 0009).
    if (auto* button = documentTabs()->tabButton(0, QTabBar::RightSide)) {
        button->hide();
    }
    documentTabs()->setCurrentIndex(session_.homeActive()
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
        leftStack()->setCurrentIndex(0);
        leftDock()->setWindowTitle(tr("Model"));
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
    leftDock()->setWindowTitle(tr("Properties"));
    leftStack()->setCurrentIndex(1);

    // Focus the first field so a command can be driven from the keyboard without reaching for the
    // mouse, which is how anyone fast actually works.
    if (commandFields_->rowCount() > 0) {
        if (auto* first = commandFields_->itemAt(0, QFormLayout::FieldRole)) {
            if (auto* w = first->widget()) w->setFocus();
        }
    }
}

void MainWindow::buildBrowserAndProperties() {
    // The docks themselves are the frame's; the feature tree and the property table are ours.
    auto* browserDock = leftDock();
    browserDock->setWindowTitle(tr("Model"));
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
    leftStack()->addWidget(browser_);          // index 0
    commandPanel_ = buildCommandPanel();
    leftStack()->addWidget(commandPanel_);     // index 1

    browser_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(browser_, &QTreeWidget::customContextMenuRequested, this,
            [this](const QPoint& at) { showBrowserMenu(at); });

    connect(browser_, &QTreeWidget::itemSelectionChanged, this, [this] {
        if (updatingUi_) return;
        auto* c = controller();
        if (c == nullptr) return;

        // Read the rows BEFORE touching the controller. This is why selecting in the browser never
        // worked: clearSelection() notifies, the notification runs refreshTree() synchronously, and
        // refreshTree() begins with browser_->clear() -- which destroys the very items the loop was
        // about to read. The loop then found an empty list and selected nothing, so a click in the
        // Model panel left the selection empty and the viewport unmarked.
        std::vector<cad::document::ObjectId> ids;
        for (auto* item : browser_->selectedItems()) {
            const auto id = item->data(0, Qt::UserRole).toULongLong();
            if (id != 0) ids.push_back(cad::document::ObjectId{id});
        }
        // One call, so one notification and one tree rebuild -- rather than N+1 of each.
        c->setSelection(std::move(ids));
    });

    auto* propertiesDock = rightDock();
    properties_ = new QTableWidget(propertiesDock);
    properties_->setColumnCount(2);
    properties_->setHorizontalHeaderLabels({tr("Property"), tr("Value")});
    properties_->horizontalHeader()->setStretchLastSection(true);
    properties_->verticalHeader()->setVisible(false);
    properties_->setSelectionBehavior(QAbstractItemView::SelectRows);
    propertiesDock->setWidget(properties_);

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

void MainWindow::buildStatusFields() {
    // The message label is the frame's; these two are vCAD's, and read right to left as
    // Inventor's do.
    statusStats_ = new QLabel(this);
    statusUnits_ = new QLabel(tr("mm"), this);
    addStatusField(statusStats_);
    addStatusField(statusUnits_);
}

// ── refresh ─────────────────────────────────────────────────────────────────────────────

void MainWindow::showPluginManager() {
    // One window, reused. Reopening from the ribbon raises the existing one rather than stacking
    // copies -- and it is modeless, because reading a plugin list is something you do WHILE
    // deciding what to do next, not a question you must answer before continuing.
    if (pluginManager_ == nullptr) {
        pluginManager_ = new PluginManager(this);
    }
    pluginManager_->show();
    pluginManager_->raise();
    pluginManager_->activateWindow();
}

void MainWindow::openPluginManagerForShot() { showPluginManager(); }

void MainWindow::drawSketchForShot(int lines) {
    auto* c = controller();
    if (c == nullptr) return;
    c->beginSketch();
    c->setSketchTool(cad::app::Controller::SketchTool::Line);
    // Device pixels, as the viewport passes them.
    const std::size_t index = session_.activeIndex();
    if (index >= editors_.size()) return;
    auto* view = qobject_cast<Viewport*>(editors_[index]);
    if (view == nullptr) return;
    const float dpr = static_cast<float>(view->devicePixelRatioF());
    const float w = static_cast<float>(view->width()) * dpr;
    const float h = static_cast<float>(view->height()) * dpr;
    // `lines` is how many segments to draw. ZERO leaves the seeded rectangle closed, which is the
    // only way to photograph the shaded profile — adding any stray line opens it, and an open
    // profile deliberately shades nothing.
    if (lines > 0) {
        // A chain, the way a person draws: click, click, click. Not two clicks per segment.
        c->sketchClickAt(w * 0.30f, h * 0.30f);
        c->sketchClickAt(w * 0.65f, h * 0.30f);
        c->sketchClickAt(w * 0.65f, h * 0.60f);
    }
    if (lines > 1) {
        // Mid-chain, pointer moved: the state the rubber band and the dimension field exist in.
        c->sketchHoverAt(w * 0.40f, h * 0.62f);
        // Locked, so the shot catches the padlock and a band held at a fixed length while the
        // pointer aims elsewhere — the whole point of Tab, and invisible in any other state.
        c->typeSketchDimension('6');
        c->typeSketchDimension('0');
        c->lockSketchDimension();
        c->sketchHoverAt(w * 0.34f, h * 0.70f);
    }
    view->syncDimensionFieldForShot();
    view->markDirty();
}

QPixmap MainWindow::grabPluginManager() {
    return pluginManager_ != nullptr ? pluginManager_->grab() : grab();
}

void MainWindow::loadPlugins() {
    if (pluginSession_ != 0) return;
    pluginSession_ = cad_session_create();
    if (pluginSession_ == 0) return;

    // Beside the executable, the same reasoning as the shaders and the log: vCAD ships as a bare
    // binary, so resolving against the working directory would mean plugins load only when the app
    // is started from the build directory. CAD_PLUGIN_DIR overrides it, which is what developing a
    // plugin needs and what a support conversation can ask for.
    std::filesystem::path root;
    if (const char* fromEnv = std::getenv("CAD_PLUGIN_DIR")) {
        if (*fromEnv != '\0') root = fromEnv;
    }
    if (root.empty()) {
        root = std::filesystem::path(QCoreApplication::applicationDirPath().toStdString()) /
               "plugins";
    }

    std::uint32_t loaded = 0;
    std::uint32_t failed = 0;
    cad_plugins_load(pluginSession_, root.string().c_str(), &loaded, &failed);

    if (loaded > 0) {
        CAD_INFO(::cad::log::Category::Shell)
            << "loaded " << loaded << " plugin(s) from " << root.string();
    }
    if (failed > 0) {
        // Said out loud, in the window, not only in the log. A plugin that silently did not load is
        // the complaint every CAD plugin system generates -- the user sees a missing command and has
        // no way to find out why. One failure gets its reason; several get a count and a pointer at
        // the manager, which lists every installed plugin without loading any of them.
        const char* reason = cad_session_last_error(pluginSession_);
        const QString detail = (failed == 1 && reason != nullptr && *reason != '\0')
                                   ? tr("Plugin not loaded: %1").arg(QString::fromUtf8(reason))
                                   : tr("%1 plugins could not be loaded — see Plugins.").arg(failed);
        CAD_WARN(::cad::log::Category::Shell) << detail.toStdString();
        pluginLoadWarning_ = detail;
    }
}

void MainWindow::setOrbitMode(bool on) {
    auto* c = controller();
    if (c == nullptr) return;
    c->setOrbitMode(on);
    // Both controls follow the CONTROLLER rather than each other. Two checkable widgets for one
    // mode is exactly where a UI drifts out of step with itself, and a button showing Orbit while
    // dragging selects is worse than no button.
    if (orbitButton_ != nullptr) orbitButton_->setChecked(on);
    if (orbitAction_ != nullptr) orbitAction_->setChecked(on);
    setStatusMessage(on ? tr("Orbit: drag to rotate the view")
                        : tr("Orbit off — drag selects again"));
}

void MainWindow::refreshSketchToolStates() {
    if (auto* active = controller()) {
        const bool inSketch = active->environment() == cad::app::Environment::Sketch;
        for (QAction* action : sketchViewActions_) action->setEnabled(inSketch);
        // Followed from the CONTROLLER, which turns slice off when a sketch closes. A button left
        // lit for a mode that is no longer on is how a user stops trusting the toolbar.
        if (sliceAction_ != nullptr) sliceAction_->setChecked(active->sliceEnabled());
    }

    auto* c = controller();
    const bool sketching = c != nullptr && c->environment() == cad::app::Environment::Sketch;
    for (const auto& [action, tool] : sketchToolActions_) {
        action->setEnabled(sketching);
        // Checked from the CONTROLLER rather than from which button was last pressed: the tool can
        // also be reset by finishing a sketch, and a toolbar showing Line still lit after the
        // sketch closed is how a user ends up wondering why clicking does nothing.
        action->setChecked(sketching && c->sketchTool() == tool);
    }
}

void MainWindow::declareSettings() {
    if (settings_ != nullptr) return;
    // Before the pages are built, because addPluginSettings below reads what loading registered.
    loadPlugins();
    // Deferred to the event loop: declareSettings can run before the window is shown, and a status
    // message posted to a status bar nobody is looking at yet is a message the user never sees.
    if (!pluginLoadWarning_.isEmpty()) {
        QTimer::singleShot(0, this, [this] {
            statusBar()->showMessage(pluginLoadWarning_, 15000);
        });
    }
    settings_ = new proshell::Settings(QStringLiteral("vCAD"), QStringLiteral("vCAD"), this);

    // The ids are PERMANENT — they are what the user's stored preferences are keyed by, so renaming
    // one silently discards whatever they had chosen. The labels are free to reword and translate.
    // Same rule as a ribbon section id, a sketch parameter name and a plugin feature type: anything
    // persisted is permanent, anything displayed is not.
    proshell::SettingsPage general;
    general.id = QStringLiteral("vcad.general");
    general.label = tr("General");
    general.iconName = QStringLiteral("parameters");

    proshell::SettingsGroup display;
    display.label = tr("Display");

    proshell::Setting units;
    units.id = QStringLiteral("general.displayUnits");
    units.label = tr("Display units");
    units.description = tr("How lengths are shown and how a bare number is read. Storage is always "
                           "millimetres — this never changes what is in the file.");
    units.kind = proshell::SettingKind::Choice;
    // Order matches units::UnitSystem so the index IS the enum value. Kept adjacent to that fact
    // rather than mapped, because a mapping table is one more thing to forget to update.
    units.choices = {tr("Millimetres"), tr("Centimetres"), tr("Metres"), tr("Inches"), tr("Feet")};
    units.fallback = 0;
    display.settings.push_back(units);

    proshell::Setting navigation;
    navigation.id = QStringLiteral("general.navigation");
    navigation.label = tr("Navigation");
    navigation.description = tr("Which mouse button orbits. Match whichever application you came "
                                "from — this is muscle memory, not preference.");
    navigation.kind = proshell::SettingKind::Choice;
    navigation.choices = {tr("CAD (middle drag orbits)"), tr("Fusion (middle drag pans)"),
                          tr("Blender")};
    navigation.fallback = 0;
    display.settings.push_back(navigation);

    general.groups.push_back(display);
    settings_->addPage(general);

    proshell::SettingsPage sketch;
    sketch.id = QStringLiteral("vcad.sketch");
    sketch.label = tr("Sketch");
    sketch.iconName = QStringLiteral("sketch");

    proshell::SettingsGroup inference;
    inference.label = tr("Constraint inference");

    proshell::Setting snap;
    snap.id = QStringLiteral("sketch.snapTolerance");
    snap.label = tr("Snap tolerance");
    snap.description = tr("Endpoints closer than this are treated as one point when inferring "
                          "constraints from an imported file.");
    snap.kind = proshell::SettingKind::Double;
    snap.minimum = 0.0001;
    snap.maximum = 100.0;
    snap.fallback = 0.01;
    inference.settings.push_back(snap);

    proshell::Setting angle;
    angle.id = QStringLiteral("sketch.angleTolerance");
    angle.label = tr("Angle tolerance (degrees)");
    angle.description = tr("A line within this of an axis is inferred horizontal or vertical. Keep "
                           "it small: a 2° taper is design intent, not a drafting error.");
    angle.kind = proshell::SettingKind::Double;
    angle.minimum = 0.0;
    angle.maximum = 45.0;
    angle.fallback = 0.5;
    inference.settings.push_back(angle);

    sketch.groups.push_back(inference);
    settings_->addPage(sketch);

    proshell::SettingsPage appearance;
    appearance.id = QStringLiteral("vcad.appearance");
    appearance.label = tr("Appearance");
    appearance.iconName = QStringLiteral("view");

    proshell::SettingsGroup colours;
    colours.label = tr("Colour scheme");

    proshell::Setting theme;
    theme.id = QStringLiteral("appearance.theme");
    theme.label = tr("Theme");
    theme.description = tr("Paper White is the reference scheme. The others are alternatives to it, "
                           "not replacements — it is deliberately soft rather than stark.");
    theme.kind = proshell::SettingKind::Choice;
    // From proshell, in Theme order, so the stored INDEX is the enum value and no mapping table has
    // to be kept in step with the enum.
    theme.choices = proshell::themeNames();
    theme.fallback = static_cast<int>(proshell::Theme::PaperWhite);
    colours.settings.push_back(theme);

    appearance.groups.push_back(colours);
    settings_->addPage(appearance);

    // AFTER the built-in pages, so a plugin's page appears below them rather than interleaved, and
    // so a plugin cannot displace a built-in setting: addPage merges by page id and DROPS a
    // duplicate setting id, and the first one registered is the one that survives.
    addPluginSettings();

    connect(settings_, &proshell::Settings::changed, this,
            [this](const QString& id, const QVariant&) { applySetting(id); });
}

void MainWindow::addPluginSettings() {
    if (settings_ == nullptr || pluginSession_ == 0) return;

    std::uint32_t pageCount = 0;
    if (cad_settings_page_count(pluginSession_, &pageCount) != CAD_OK) return;

    for (std::uint32_t p = 0; p < pageCount; ++p) {
        CadSettingsPageDesc desc{};
        desc.struct_size = sizeof(desc);
        std::uint32_t settingCount = 0;
        if (cad_settings_page_at(pluginSession_, p, &desc, &settingCount) != CAD_OK) continue;
        if (desc.id == nullptr || *desc.id == '\0') continue;

        proshell::SettingsPage page;
        page.id = QString::fromUtf8(desc.id);
        page.label = desc.label != nullptr ? QString::fromUtf8(desc.label) : page.id;
        // An icon the theme does not know resolves to a placeholder rather than nothing, which is
        // why a plugin naming one we have never heard of is not checked here.
        page.iconName = desc.icon_name != nullptr ? QString::fromUtf8(desc.icon_name) : QString();

        proshell::SettingsGroup group;
        group.label = desc.group_label != nullptr ? QString::fromUtf8(desc.group_label) : QString();

        for (std::uint32_t i = 0; i < settingCount; ++i) {
            CadSettingDesc sd{};
            sd.struct_size = sizeof(sd);
            if (cad_settings_at(pluginSession_, p, i, &sd) != CAD_OK) continue;
            if (sd.id == nullptr || *sd.id == '\0') continue;

            proshell::Setting setting;
            setting.id = QString::fromUtf8(sd.id);
            setting.label = sd.label != nullptr ? QString::fromUtf8(sd.label) : setting.id;
            setting.description =
                sd.description != nullptr ? QString::fromUtf8(sd.description) : QString();
            setting.minimum = sd.minimum;
            setting.maximum = sd.maximum;

            switch (sd.kind) {
                case CAD_SETTING_INT:
                    setting.kind = proshell::SettingKind::Int;
                    setting.fallback = static_cast<int>(sd.default_value);
                    break;
                case CAD_SETTING_DOUBLE:
                    setting.kind = proshell::SettingKind::Double;
                    setting.fallback = sd.default_value;
                    break;
                case CAD_SETTING_TEXT:
                    setting.kind = proshell::SettingKind::Text;
                    setting.fallback = sd.default_text != nullptr
                                           ? QString::fromUtf8(sd.default_text) : QString();
                    break;
                case CAD_SETTING_CHOICE: {
                    setting.kind = proshell::SettingKind::Choice;
                    for (std::uint32_t c = 0; c < sd.choice_count; ++c) {
                        if (sd.choices == nullptr || sd.choices[c] == nullptr) continue;
                        setting.choices << QString::fromUtf8(sd.choices[c]);
                    }
                    // The INDEX is stored, so an out-of-range default would select nothing and the
                    // combo would show blank. Clamped rather than refused: a plugin with one bad
                    // default should still get a usable page.
                    const int fallbackIndex = static_cast<int>(sd.default_value);
                    setting.fallback = (fallbackIndex >= 0 && fallbackIndex < setting.choices.size())
                                           ? fallbackIndex : 0;
                    break;
                }
                case CAD_SETTING_BOOL:
                default:
                    // Unknown kinds fall back to a checkbox rather than being dropped: a plugin
                    // built against a LATER header than this shell should lose fidelity, not its
                    // whole page. Same additive rule as the descriptor structs.
                    setting.kind = proshell::SettingKind::Bool;
                    setting.fallback = sd.default_value != 0.0;
                    break;
            }
            group.settings.push_back(std::move(setting));
        }

        if (group.settings.empty()) continue;   // an empty page is a dead entry in the list
        page.groups.push_back(std::move(group));
        settings_->addPage(std::move(page));
    }
}

void MainWindow::applySetting(const QString& id) {
    if (settings_ == nullptr) return;

    if (id == QLatin1String("appearance.theme")) {
        // Applied to the whole application, not this window: menus, dialogs and the settings window
        // itself are all painted by the same palette, and repainting one of them would look broken.
        if (auto* app = qobject_cast<QApplication*>(QCoreApplication::instance())) {
            proshell::applyTheme(*app,
                                 static_cast<proshell::Theme>(settings_->integer(id)));
        }
        setStatusMessage(tr("Theme applied"));
        return;
    }

    cad::app::Preferences next;
    next.displayUnits = static_cast<cad::units::UnitSystem>(
        settings_->integer(QStringLiteral("general.displayUnits")));
    next.navigation = static_cast<cad::render::NavigationPreset>(
        settings_->integer(QStringLiteral("general.navigation")));
    next.snapTolerance = settings_->real(QStringLiteral("sketch.snapTolerance"), 0.01);
    next.angleTolerance = settings_->real(QStringLiteral("sketch.angleTolerance"), 0.5);

    // Every open document. Units belong to the user, not to a file.
    const std::size_t active = session_.count() > 0 ? session_.activeIndex() : 0;
    for (std::size_t i = 0; i < session_.count(); ++i) {
        session_.activate(i);
        if (auto* ctl = session_.active()) ctl->setPreferences(next);
    }
    if (session_.count() > 0) session_.activate(active);

    refreshProperties();
    refreshStatus();
    setStatusMessage(tr("Applied %1").arg(id));
}

void MainWindow::restoreTheme() {
    // At startup, so the window is painted in the user's theme rather than flashing the default
    // first. Declaring the settings is what makes the stored value readable at all.
    declareSettings();
    if (auto* app = qobject_cast<QApplication*>(QCoreApplication::instance())) {
        proshell::applyTheme(*app, static_cast<proshell::Theme>(
                                       settings_->integer(QStringLiteral("appearance.theme"))));
    }
}

QPixmap MainWindow::grabSettingsForShot(const QString& pageId) {
    // Shown rather than exec'd: exec blocks in its own event loop, so a screenshot driver would
    // never get control back. Same reason the document is created inside a singleShot.
    declareSettings();
    auto* window = new proshell::SettingsWindow(*settings_, this);
    if (!pageId.isEmpty()) window->showPage(pageId);
    window->show();
    QApplication::processEvents();
    QApplication::processEvents();   // the second turn is where layout lands, as with the main grab
    const QPixmap shot = window->grab();
    window->deleteLater();
    return shot;
}

void MainWindow::showOptions() {
    // Replaces a hand-built QFormLayout that read and wrote Controller::preferences() directly and
    // persisted NOTHING — every preference reset on restart, which docs/STATUS.md recorded as an
    // outstanding gap. The store persists them, so this fixes that as a side effect of being
    // declarative rather than as a separate feature.
    declareSettings();
    proshell::SettingsWindow window(*settings_, this);
    window.exec();
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

        // The Origin folder, created only when there is something to put in it, and COLLAPSED.
        //
        // Datums are reference geometry, not history. Left loose in the list they sit between the
        // features as three steps the user does not remember performing — which is exactly how they
        // were first reported. Collapsed by default for the same reason Fusion collapses its Origin
        // folder: they are always there and rarely the thing you came to the tree for.
        QTreeWidgetItem* originFolder = nullptr;
        const auto parentFor = [&](const cad::app::TreeItem& item) -> QTreeWidgetItem* {
            if (item.group != cad::app::TreeGroup::Origin) return root;
            if (originFolder == nullptr) {
                originFolder = new QTreeWidgetItem(root);
                originFolder->setText(0, tr("Origin"));
                originFolder->setIcon(0, icon(QStringLiteral("origin"), 16));
                originFolder->setExpanded(false);
                // No object id: the folder is a grouping, not a thing that can be selected,
                // renamed or deleted. Without this a click on it would carry a stale id.
                originFolder->setData(0, Qt::UserRole, QVariant());
            }
            return originFolder;
        };

        for (const auto& item : c->tree()) {
            auto* node = new QTreeWidgetItem(parentFor(item));
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
        QString text;

        // The live dimension FIRST, and in the status bar as well as beside the cursor.
        //
        // The floating readout is a top-level window composited over a Metal layer, and that is the
        // one part of this shell my own screenshot harness cannot exercise — the first attempt at it
        // turned the viewport black. The status bar is an ordinary widget: if the floating copy ever
        // misbehaves again, the numbers are still here, and the feature degrades to "less
        // convenient" rather than "gone".
        const auto measure = c->sketchPreviewText();
        if (measure.valid) {
            text += QString::fromStdString(measure.length);
            if (!measure.angle.empty()) {
                text += QStringLiteral("  ") + QString::fromStdString(measure.angle);
            }
            if (c->sketchLockedLength()) text += tr(" (locked)");
            text += QStringLiteral("   ·   ");
        }

        text += tr("%1 curves · %2 constraints · ").arg(curves).arg(constraints);
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
