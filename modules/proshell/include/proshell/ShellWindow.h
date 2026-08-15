#pragma once

/// The frame every professional application has, without the application in it.
///
/// # What this is
///
/// Quick access strip, ribbon, a dockable panel each side, a workspace stack with document tabs
/// along the bottom, and a status bar. That layout is not vCAD's invention and not Inventor's
/// either — it is what SolidWorks, Inventor, Revit, Civil 3D and every tool in that family
/// converged on, and an application in a new domain should start from it rather than rediscover it.
///
/// # What it deliberately is NOT
///
/// There is **no document model here.** No `IDocumentHost`, no virtual `documentCount()`, no
/// abstract notion of dirty or saved. The subclass owns its documents and wires the tab bar
/// itself, in about a dozen lines.
///
/// That was a decision, not an omission. A document interface written now would be shaped entirely
/// by vCAD — the only application that exists — and the second application would inherit its
/// assumptions rather than be served by them. Sessions, environments, and "Home is not a document"
/// are all vCAD's ideas about documents, and an architecture tool may not share any of them. So
/// this class hands the subclass the *widgets* and stays out of the way. When two applications
/// have both wired the tab bar and want the same thing, the shape of that thing will be a fact
/// rather than a guess, and it can be added then without breaking anyone.
///
/// # How a subclass uses it
///
/// Construct, then call `buildChrome()` once the subclass's own state exists — it is not called
/// from this constructor precisely because the hooks it runs would then see a half-built subclass.
/// After that, populate through the protected accessors:
///
/// ```
/// MyWindow::MyWindow() {
///     setProductName("MyApp");
///     buildChrome();
///     addQuickAccessButton("new", tr("New"), [this]{ newDocument(); }, QKeySequence::New);
///     fileMenu()->addAction(tr("Exit"), this, &QWidget::close);
///     setSidebar(myHomeRail, 260);
///     leftStack()->addWidget(myBrowser);
///     rightDock()->setWidget(myProperties);
/// }
/// ```

#include <QDockWidget>
#include <QKeySequence>
#include <QMainWindow>
#include <QString>

#include <functional>

class QLabel;
class QMenu;
class QSplitter;
class QStackedWidget;
class QTabBar;
class QToolButton;

namespace proshell {

class Ribbon;

class ShellWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit ShellWindow(QWidget* parent = nullptr);
    ~ShellWindow() override;

protected:
    /// Builds the whole frame. Call once, from the subclass constructor, after the subclass's own
    /// members exist. Calling it twice is a no-op rather than a second window's worth of widgets.
    void buildChrome();
    [[nodiscard]] bool chromeBuilt() const noexcept { return built_; }

    // ── the top strip ────────────────────────────────────────────────────────────────────
    //
    // ONE menu widget holds both the quick access strip and the ribbon. Setting the ribbon with
    // setMenuWidget() and then calling menuBar() REPLACES the menu widget, which destroys the
    // ribbon at startup — and on macOS, where QMenuBar goes to the global bar, leaves a window
    // showing no commands at all. There is no QMenuBar here: Inventor has none either, it has a
    // File tab in the strip.

    [[nodiscard]] Ribbon* ribbon() noexcept { return ribbon_; }
    /// The menu behind the File tab. Empty until the subclass fills it.
    [[nodiscard]] QMenu* fileMenu() noexcept { return fileMenu_; }

    /// Adds a button to the quick access strip, left of the stretch.
    QToolButton* addQuickAccessButton(const QString& iconName, const QString& text,
                                      std::function<void()> onClick,
                                      const QKeySequence& shortcut = {});
    /// A gap between groups of quick access buttons.
    void addQuickAccessSpacing(int pixels = 6);
    /// Adds a widget to the RIGHT of the quick access strip, before the product name. For controls
    /// that belong to the strip but are not commands — a selection filter, a units picker.
    void addQuickAccessWidget(QWidget*);

    /// Shown greyed at the right of the strip. Set before `buildChrome()`.
    void setProductName(QString);

    // ── the workspace area ───────────────────────────────────────────────────────────────

    /// Pages stack here — a home page, one editor per document. The subclass adds and removes.
    [[nodiscard]] QStackedWidget* workspaces() noexcept { return workspaces_; }

    /// The tab bar along the BOTTOM, as Inventor has. Unwired: the subclass connects
    /// `currentChanged` and `tabCloseRequested` to whatever its documents are.
    [[nodiscard]] QTabBar* documentTabs() noexcept { return documentTabs_; }

    /// A full-height rail down the left of the workspace area, outside the page stack.
    ///
    /// Full height matters and is the reason this is not simply another page: a rail that stops
    /// above the document tab bar reads as a panel sitting inside the page rather than as a
    /// sidebar beside it. Passing null removes whatever rail is there.
    void setSidebar(QWidget*, int defaultWidth);
    /// Shows or hides the rail without discarding it or the width the user dragged it to.
    void setSidebarVisible(bool);

    // ── docks ────────────────────────────────────────────────────────────────────────────

    [[nodiscard]] QDockWidget* leftDock() noexcept { return leftDock_; }
    [[nodiscard]] QDockWidget* rightDock() noexcept { return rightDock_; }

    /// The left dock holds EITHER its primary panel or something that has taken the space over,
    /// never both — SolidWorks' PropertyManager pattern. A stack rather than a splitter because
    /// the two are alternatives, and a squeezed tree beside a squeezed command panel is the worst
    /// of both. The subclass adds its own widgets and chooses the index.
    [[nodiscard]] QStackedWidget* leftStack() noexcept { return leftStack_; }

    // ── status bar ───────────────────────────────────────────────────────────────────────

    void setStatusMessage(const QString&);
    [[nodiscard]] QString statusMessage() const;
    /// A field pinned to the right of the status bar — units, counts, a mode indicator. Added
    /// left to right in call order.
    void addStatusField(QWidget*);

    // ── closing ──────────────────────────────────────────────────────────────────────────

    /// Asked before the window closes. Return false to cancel. Default accepts.
    ///
    /// A hook rather than an override of closeEvent so a subclass cannot forget to call the base
    /// and silently lose the frame's own teardown.
    [[nodiscard]] virtual bool confirmClose() { return true; }
    void closeEvent(QCloseEvent*) final;

private:
    void buildTopStrip();
    void buildWorkspaceArea();
    void buildDocks();
    void buildStatus();

    bool built_ = false;
    QString productName_;

    Ribbon* ribbon_ = nullptr;
    QMenu* fileMenu_ = nullptr;
    QWidget* quickAccessRow_ = nullptr;
    /// Marks where `addQuickAccessButton` inserts: everything after it is the stretch, the
    /// right-hand widgets and the product label.
    int quickAccessInsertAt_ = 0;
    QLabel* productLabel_ = nullptr;

    QSplitter* splitter_ = nullptr;
    QWidget* sidebar_ = nullptr;
    int sidebarWidth_ = 0;
    QStackedWidget* workspaces_ = nullptr;
    QTabBar* documentTabs_ = nullptr;

    QDockWidget* leftDock_ = nullptr;
    QDockWidget* rightDock_ = nullptr;
    QStackedWidget* leftStack_ = nullptr;

    QLabel* statusMessage_ = nullptr;
};

}  // namespace proshell
