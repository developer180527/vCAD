#include "proshell/ShellWindow.h"

#include "proshell/Icons.h"
#include "proshell/Ribbon.h"

#include <QCloseEvent>
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
    buildTopStrip();
    buildWorkspaceArea();
    buildDocks();
    buildStatus();
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
    // Before the product label, after the stretch: the right-hand group.
    row->insertWidget(row->count() - 1, widget);
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
