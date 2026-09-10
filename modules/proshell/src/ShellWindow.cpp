#include "proshell/ShellWindow.h"

#include "proshell/Icons.h"
#include "proshell/Ribbon.h"
#include "proshell/WindowChrome.h"

#include <QApplication>
#include <QCloseEvent>
#include <QEvent>
#include <QMouseEvent>
#include <QShowEvent>
#include <QTimer>
#include <QWindow>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTabBar>
#include <QToolButton>
#include <QVBoxLayout>

namespace proshell {
namespace {

/// A QTabBar that tells its parent layout when its tab count changes.
///
/// Working around a real Qt behaviour, not a style preference. `QTabBarPrivate::refresh()` skips
/// `updateGeometry()` when the bar is not visible -- it only sets an internal dirty flag. So tabs
/// added during construction, before the window is shown, never invalidate the cached size the
/// parent layout's QWidgetItem is holding. That cache was taken when the bar was empty, and an
/// empty QTabBar has a size hint of 0x0.
///
/// The result is a tab bar that is laid out at zero height and simply is not there, while every
/// property you would think to check looks correct: it is visible, it is in the layout, its own
/// sizeHint is 29 pixels. Only the layout ITEM reports zero.
///
/// `tabInserted`/`tabRemoved` are the documented hooks for exactly this, and doing it here means
/// no consumer has to know the order in which it may safely populate the bar.
class DocumentTabBar : public QTabBar {
public:
    using QTabBar::QTabBar;

protected:
    void tabInserted(int index) override {
        QTabBar::tabInserted(index);
        updateGeometry();
    }
    void tabRemoved(int index) override {
        QTabBar::tabRemoved(index);
        updateGeometry();
    }
};

}  // namespace

ShellWindow::ShellWindow(QWidget* parent) : QMainWindow(parent) {}
ShellWindow::~ShellWindow() = default;

void ShellWindow::buildChrome() {
    if (built_) return;
    built_ = true;
    // BEFORE the strip is built, and before the window is ever shown. Creating the native window
    // here and changing its style mask now means Qt sizes its view against a window that already
    // has no title bar to subtract -- doing it later left the view laid out for the old content
    // rect, and the strip sat below a bar that was no longer drawn.
    (void)winId();
    adoptTitleBar(this, palette().color(QPalette::Window));
    buildTopStrip();
    buildWorkspaceArea();
    buildDocks();
    buildStatus();

    // RESIZE, where the window is frameless. The frame took its resize edges with it, and nothing
    // gave them back: the platform file promised that ShellWindow called startSystemResize, and
    // nothing did, so on Windows and Linux the window was stuck at the size it opened at.
    //
    // On the APPLICATION rather than on the window, because a press near the edge lands on whatever
    // child happens to be there -- the ribbon, a dock, the viewport -- and never reaches the window
    // itself. Not installed on macOS, where the system's own frame still resizes.
    if (!hasSystemWindowButtons()) qApp->installEventFilter(this);
}

void ShellWindow::setProductName(QString name) {
    productName_ = std::move(name);
    if (productLabel_ != nullptr) productLabel_->setText(productName_);
}

void ShellWindow::buildTopStrip() {
    auto* top = new QWidget(this);
    auto* column = new QVBoxLayout(top);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(0);

    quickAccessRow_ = new QWidget(top);
    quickAccessRow_->setObjectName(QStringLiteral("qat"));
    auto* row = new QHBoxLayout(quickAccessRow_);
    row->setContentsMargins(0, 0, 8, 0);
    row->setSpacing(2);

    // Room for the window buttons the system draws over this strip. On macOS those are the traffic
    // lights, which stay exactly where the system puts them: this is the application getting out of
    // their way, not placing them. Zero-width and harmless where there are none.
    systemButtonGap_ = new QWidget(quickAccessRow_);
    systemButtonGap_->setFixedWidth(hasSystemWindowButtons() ? systemButtonInset(this) : 0);
    row->addWidget(systemButtonGap_);

    auto* fileTab = new QToolButton(quickAccessRow_);
    fileTab->setText(tr("File"));
    fileTab->setObjectName(QStringLiteral("fileTab"));
    fileTab->setPopupMode(QToolButton::InstantPopup);
    fileMenu_ = new QMenu(fileTab);
    fileTab->setMenu(fileMenu_);
    row->addWidget(fileTab);
    row->addSpacing(6);

    // Everything the subclass adds goes in front of this stretch; everything the frame owns goes
    // behind it. Tracking the index rather than appending is what lets `addQuickAccessButton` and
    // `addQuickAccessWidget` be called in any order, at any time, and still land on the correct
    // side — a strip that could only be filled during construction would force the subclass to
    // build its commands before it has them.
    row->addStretch(1);
    quickAccessInsertAt_ = row->count() - 1;

    productLabel_ = new QLabel(productName_, quickAccessRow_);
    productLabel_->setStyleSheet(QStringLiteral("color: #6c7075;"));
    row->addWidget(productLabel_);

    if (!hasSystemWindowButtons()) buildWindowButtons(row);

    // The strip is what is left of the title bar, so it does what a title bar did: drag the window,
    // and zoom on a double click. Both go through the compositor rather than through mouse deltas,
    // which is what keeps snapping, edge tiling and multi-monitor DPI changes working.
    quickAccessRow_->installEventFilter(this);

    column->addWidget(quickAccessRow_);

    ribbon_ = new Ribbon(top);
    column->addWidget(ribbon_);

    // A hairline closing the ribbon off from the workspace below. Both Inventor and SolidWorks
    // separate the command area from the graphics area this way, and without it the ribbon and the
    // workspace read as one undifferentiated surface. QFrame::HLine is not used: it draws a
    // two-tone bevel that looks like a 1990s group box. This is one device pixel of the theme's
    // own line colour.
    auto* rule = new QWidget(top);
    rule->setFixedHeight(1);
    rule->setAutoFillBackground(true);
    QPalette rulePalette = rule->palette();
    rulePalette.setColor(QPalette::Window, QColor(0xcf, 0xcd, 0xc9));
    rule->setPalette(rulePalette);
    column->addWidget(rule);

    setMenuWidget(top);
}

QToolButton* ShellWindow::addQuickAccessButton(const QString& iconName, const QString& text,
                                               std::function<void()> onClick,
                                               const QKeySequence& shortcut) {
    auto* row = qobject_cast<QHBoxLayout*>(quickAccessRow_->layout());
    auto* button = new QToolButton(quickAccessRow_);
    button->setIcon(icon(iconName, 18));
    button->setIconSize(QSize(18, 18));
    button->setToolTip(shortcut.isEmpty()
                           ? text
                           : text + QStringLiteral(" (")
                                 + shortcut.toString(QKeySequence::NativeText)
                                 + QStringLiteral(")"));
    button->setObjectName(QStringLiteral("qatButton"));
    button->setAutoRaise(true);
    if (onClick) connect(button, &QToolButton::clicked, this, std::move(onClick));
    row->insertWidget(quickAccessInsertAt_++, button);
    return button;
}

void ShellWindow::addQuickAccessSpacing(int pixels) {
    auto* row = qobject_cast<QHBoxLayout*>(quickAccessRow_->layout());
    row->insertSpacing(quickAccessInsertAt_++, pixels);
}

void ShellWindow::addQuickAccessWidget(QWidget* widget) {
    if (widget == nullptr) return;
    auto* row = qobject_cast<QHBoxLayout*>(quickAccessRow_->layout());
    // Before the PRODUCT LABEL, found by identity rather than by counting back from the end.
    //
    // It was `count() - 1`, which meant "before the last thing" and was the same position only for
    // as long as the label WAS the last thing. Adding window buttons behind it put every widget the
    // application added afterwards between the maximise and close buttons -- the selection filter
    // came up with a minimise and a maximise to its left and a close to its right, which is exactly
    // what a hand-rolled title bar looks like when it goes wrong.
    row->insertWidget(row->indexOf(productLabel_), widget);
}

/// Minimise, maximise and close, drawn here because the platform draws none.
///
/// Trailing edge, which is where Windows and the common Linux desktops put them. macOS never
/// reaches this: its buttons are real system buttons at the LEADING edge, and moving them to match
/// would be a worse kind of consistency -- every user of the platform reaches for the corner their
/// system uses, not the corner this application prefers.
///
/// Text glyphs rather than icons. They are the same three shapes on every desktop, they scale with
/// the font, and they need no artwork to go stale.
void ShellWindow::buildWindowButtons(QHBoxLayout* row) {
    struct Button {
        QString glyph;
        QString tip;
        const char* name;
    };
    const Button buttons[] = {
        {QStringLiteral("\u2500"), tr("Minimise"), "windowMinimise"},
        {QStringLiteral("\u25a1"), tr("Maximise"), "windowMaximise"},
        {QStringLiteral("\u2715"), tr("Close"), "windowClose"},
    };
    for (const Button& spec : buttons) {
        auto* button = new QToolButton(quickAccessRow_);
        button->setText(spec.glyph);
        button->setToolTip(spec.tip);
        button->setObjectName(QString::fromLatin1(spec.name));
        button->setAutoRaise(true);
        row->addWidget(button);
        if (spec.name == QLatin1String("windowMinimise")) {
            connect(button, &QToolButton::clicked, this, &QWidget::showMinimized);
        } else if (spec.name == QLatin1String("windowMaximise")) {
            maximiseButton_ = button;
            connect(button, &QToolButton::clicked, this, [this] { toggleMaximised(); });
        } else {
            connect(button, &QToolButton::clicked, this, &QWidget::close);
        }
    }
}

void ShellWindow::toggleMaximised() {
    if (isMaximized()) {
        showNormal();
    } else {
        showMaximized();
    }
}

void ShellWindow::changeEvent(QEvent* event) {
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::WindowStateChange) {
        // HERE, not after showMaximized(). Straight after the call isMaximized() can still report the
        // old state on some platforms, and a snap, a shortcut or the system menu changes the state
        // without going through the button at all -- each left the tooltip offering the wrong verb.
        if (maximiseButton_ != nullptr) {
            maximiseButton_->setToolTip(isMaximized() ? tr("Restore") : tr("Maximise"));
        }
    } else if (event->type() == QEvent::PaletteChange && built_) {
        // A theme switch at runtime. The band behind the traffic lights is painted by the system,
        // so it does not repaint with the widgets and has to be told.
        adoptTitleBar(this, palette().color(QPalette::Window));
    }
}

void ShellWindow::showEvent(QShowEvent* event) {
    QMainWindow::showEvent(event);
    if (firstShowHandled_) return;
    firstShowHandled_ = true;

    // The title bar was already merged, in buildChrome, against the native window `winId()` created
    // there -- this used to merge it a second time under a comment saying no native window could
    // exist yet, and both could not be the reason. What genuinely has to wait for the first show is
    // MEASURING: the traffic lights exist from here, so the room left for them can use their real
    // size instead of the assumed default.
    if (systemButtonGap_ != nullptr && hasSystemWindowButtons()) {
        systemButtonGap_->setFixedWidth(systemButtonInset(this));
    }

    // A nudge, because the style mask changes the window's CONTENT RECT and Qt can have sized its
    // view to the old one -- which left the strip below a title bar that was no longer drawn.
    //
    // Only for a window in its NORMAL state. An explicit resize of a window restored maximised or
    // fullscreen can take it out of that state; such a window is being sized by the system anyway,
    // which is the relayout the nudge exists to force.
    if (windowState() == Qt::WindowNoState) {
        QTimer::singleShot(0, this, [this] {
            if (windowState() != Qt::WindowNoState) return;
            const QSize wanted = size();
            resize(wanted.width(), wanted.height() + 1);
            resize(wanted);
        });
    }
}

/// Which window edges a point lies on, for a frameless window's resize. A few pixels, as every
/// frameless application uses: wide enough to find with a mouse, narrow enough not to steal a click
/// meant for a control at the edge.
static Qt::Edges resizeEdgesAt(const QPoint& at, const QSize& size) {
    constexpr int kGrip = 5;
    Qt::Edges edges;
    if (at.x() < kGrip) edges |= Qt::LeftEdge;
    if (at.x() >= size.width() - kGrip) edges |= Qt::RightEdge;
    if (at.y() < kGrip) edges |= Qt::TopEdge;
    if (at.y() >= size.height() - kGrip) edges |= Qt::BottomEdge;
    return edges;
}

bool ShellWindow::eventFilter(QObject* watched, QEvent* event) {
    const bool press = event->type() == QEvent::MouseButtonPress;
    const bool doubleClick = event->type() == QEvent::MouseButtonDblClick;
    if (!press && !doubleClick) return QMainWindow::eventFilter(watched, event);
    auto* mouse = static_cast<QMouseEvent*>(event);
    if (mouse->button() != Qt::LeftButton) return QMainWindow::eventFilter(watched, event);

    // Resize first, so a press at the very edge of the strip resizes rather than drags -- the edge
    // is where a user reaches for a resize, and the rest of the strip still drags.
    if (press && !hasSystemWindowButtons() && windowState() == Qt::WindowNoState) {
        auto* widget = qobject_cast<QWidget*>(watched);
        if (widget != nullptr && widget->window() == this) {
            const Qt::Edges edges =
                resizeEdgesAt(mapFromGlobal(mouse->globalPosition().toPoint()), size());
            if (edges != Qt::Edges{}) {
                if (QWindow* handle = windowHandle()) {
                    handle->startSystemResize(edges);
                    return true;
                }
            }
        }
    }

    if (watched == quickAccessRow_) {
        // Only the EMPTY parts of the strip drag the window. A press that lands on a button is that
        // button's press, and a strip that swallowed it would be a row of controls that cannot be
        // clicked -- the failure mode of every hand-rolled title bar.
        //
        // EMPTY means no control, not no widget. The product label and the gap left for the traffic
        // lights are both widgets, and both sit exactly where a title used to be -- the first place
        // anyone reaches to move a window, and the one place that did not move it.
        QWidget* hit = quickAccessRow_->childAt(mouse->position().toPoint());
        const bool onBackground = hit == nullptr || hit == productLabel_ || hit == systemButtonGap_;
        if (onBackground && press) {
            if (QWindow* handle = windowHandle()) {
                handle->startSystemMove();
                return true;
            }
        }
        if (onBackground && doubleClick) {
            // The platform's rule where it has one -- on macOS the user's own setting -- and
            // maximise-or-restore where it does not.
            if (!titleBarDoubleClicked(this)) toggleMaximised();
            return true;
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void ShellWindow::buildWorkspaceArea() {
    // Two columns: the rail, then everything else stacked over the document tabs.
    //
    // The tab bar is deliberately INSIDE the right column rather than spanning the window. With
    // one full-width bar the rail stops above it and reads as a panel inside the page; owning the
    // left column down to the status bar is what makes it a sidebar.
    //
    // A QSplitter rather than a plain layout, so the rail can be dragged. The handle is what the
    // user grabs; the rail's own min/max width bound how far it can go, which is why those belong
    // on the rail widget rather than here.
    auto* centre = new QWidget(this);
    auto* columns = new QHBoxLayout(centre);
    columns->setContentsMargins(0, 0, 0, 0);
    columns->setSpacing(0);

    splitter_ = new QSplitter(Qt::Horizontal, centre);
    splitter_->setObjectName(QStringLiteral("shellSplitter"));
    splitter_->setChildrenCollapsible(false);   // dragging must not make the rail vanish
    splitter_->setHandleWidth(4);               // 1px reads as a border and cannot be grabbed
    columns->addWidget(splitter_, 1);

    auto* right = new QWidget(splitter_);
    auto* stackColumn = new QVBoxLayout(right);
    stackColumn->setContentsMargins(0, 0, 0, 0);
    stackColumn->setSpacing(0);

    workspaces_ = new QStackedWidget(right);
    stackColumn->addWidget(workspaces_, 1);

    documentTabs_ = new DocumentTabBar(right);
    documentTabs_->setObjectName(QStringLiteral("docTabs"));
    documentTabs_->setExpanding(false);
    documentTabs_->setDrawBase(false);
    documentTabs_->setShape(QTabBar::RoundedSouth);
    stackColumn->addWidget(documentTabs_, 0);

    splitter_->addWidget(right);

    setCentralWidget(centre);
}

void ShellWindow::setSidebar(QWidget* sidebar, int defaultWidth) {
    if (sidebar_ != nullptr) {
        sidebar_->setParent(nullptr);
        sidebar_->deleteLater();
        sidebar_ = nullptr;
    }
    if (sidebar == nullptr) return;

    sidebar_ = sidebar;
    sidebarWidth_ = defaultWidth;
    splitter_->insertWidget(0, sidebar_);

    // Only the content column absorbs window resizing; the rail keeps whatever width the user
    // dragged it to, which is what every sidebar in this family of applications does.
    splitter_->setStretchFactor(0, 0);
    splitter_->setStretchFactor(1, 1);
    splitter_->setSizes({defaultWidth, 1});
}

void ShellWindow::setSidebarVisible(bool visible) {
    if (sidebar_ != nullptr) sidebar_->setVisible(visible);
}

void ShellWindow::buildDocks() {
    leftDock_ = new QDockWidget(tr("Panel"), this);
    leftDock_->setFeatures(QDockWidget::DockWidgetMovable);
    leftStack_ = new QStackedWidget(leftDock_);
    leftDock_->setWidget(leftStack_);
    addDockWidget(Qt::LeftDockWidgetArea, leftDock_);
    resizeDocks({leftDock_}, {290}, Qt::Horizontal);

    rightDock_ = new QDockWidget(tr("Properties"), this);
    rightDock_->setFeatures(QDockWidget::DockWidgetMovable);
    addDockWidget(Qt::RightDockWidgetArea, rightDock_);
    resizeDocks({rightDock_}, {300}, Qt::Horizontal);
}

void ShellWindow::buildStatus() {
    statusMessage_ = new QLabel(tr("Ready"), this);
    statusBar()->addWidget(statusMessage_, 1);
}

void ShellWindow::setStatusMessage(const QString& text) {
    if (statusMessage_ != nullptr) statusMessage_->setText(text);
}

QString ShellWindow::statusMessage() const {
    return statusMessage_ != nullptr ? statusMessage_->text() : QString();
}

void ShellWindow::addStatusField(QWidget* field) {
    if (field != nullptr) statusBar()->addPermanentWidget(field);
}

void ShellWindow::closeEvent(QCloseEvent* event) {
    // `final`, with confirmClose() as the hook. An overridable closeEvent is the shape where a
    // subclass forgets to call the base and the frame's own teardown silently stops running --
    // and the symptom of that is a window that closes without asking about unsaved work, which
    // nobody notices until it costs someone their afternoon.
    if (confirmClose()) {
        event->accept();
    } else {
        event->ignore();
    }
}

}  // namespace proshell
