#include "proshell/Ribbon.h"

#include <QFrame>
#include <QLabel>
#include <QVBoxLayout>

namespace proshell {
namespace {

constexpr int kLargeIcon = 32;
constexpr int kSmallIcon = 16;
/// Three small buttons per column is what Inventor does, and it is what makes a mixed panel line
/// up with the large buttons beside it.
constexpr int kSmallPerColumn = 3;

/// Every panel's button area is this tall, whatever it holds.
///
/// Without it a panel of three small buttons is shorter than a panel of large ones, and its
/// caption floats up to meet it — so the captions form a ragged line across the ribbon. Inventor
/// aligns them, and the alignment is most of what makes the band read as one surface rather than
/// as a row of separate toolbars. Sized for the tallest thing a panel holds: a large button with
/// a 32 px icon over a TWO-line label ("Start Sketch", "Section View"). Sizing it for one line
/// clips the second, which is how it read at 62.
constexpr int kPanelContentHeight = 72;

}  // namespace

// ── panel ───────────────────────────────────────────────────────────────────────────────

RibbonPanel::RibbonPanel(const QString& title, QWidget* parent) : QWidget(parent) {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(4, 4, 4, 2);
    outer->setSpacing(2);

    auto* content = new QWidget(this);
    content->setFixedHeight(kPanelContentHeight);
    row_ = new QHBoxLayout(content);
    row_->setContentsMargins(0, 0, 0, 0);
    row_->setSpacing(2);
    row_->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    outer->addWidget(content, 0);

    // The caption UNDER the buttons. This one detail is most of what distinguishes a ribbon from
    // a toolbar in a tab widget.
    auto* caption = new QLabel(title, this);
    caption->setAlignment(Qt::AlignCenter);
    caption->setObjectName("ribbonPanelCaption");
    outer->addWidget(caption, 0);
}

QToolButton* RibbonPanel::addLarge(QAction* action) {
    if (action == nullptr) return nullptr;   // a command the app does not expose: show nothing
    auto* button = new QToolButton(this);
    button->setDefaultAction(action);
    button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    button->setIconSize(QSize(kLargeIcon, kLargeIcon));
    button->setAutoRaise(true);
    button->setMinimumWidth(56);
    button->setMaximumWidth(96);
    button->setObjectName("ribbonLarge");
    row_->addWidget(button, 0, Qt::AlignTop);
    currentSmallColumn_ = nullptr;   // a large button ends the current small column
    smallInColumn_ = 0;
    return button;
}

QToolButton* RibbonPanel::addSmall(QAction* action) {
    if (action == nullptr) return nullptr;
    if (currentSmallColumn_ == nullptr || smallInColumn_ >= kSmallPerColumn) {
        currentSmallColumn_ = new QWidget(this);
        auto* column = new QVBoxLayout(currentSmallColumn_);
        column->setContentsMargins(0, 0, 0, 0);
        column->setSpacing(1);
        column->setAlignment(Qt::AlignTop);
        row_->addWidget(currentSmallColumn_, 0, Qt::AlignTop);
        smallInColumn_ = 0;
    }
    auto* button = new QToolButton(currentSmallColumn_);
    button->setDefaultAction(action);
    button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    button->setIconSize(QSize(kSmallIcon, kSmallIcon));
    button->setAutoRaise(true);
    button->setObjectName("ribbonSmall");
    qobject_cast<QVBoxLayout*>(currentSmallColumn_->layout())->addWidget(button);
    ++smallInColumn_;
    return button;
}

void RibbonPanel::addSeparator() {
    auto* line = new QFrame(this);
    line->setFrameShape(QFrame::VLine);
    line->setObjectName("ribbonSeparator");
    row_->addWidget(line);
    currentSmallColumn_ = nullptr;
    smallInColumn_ = 0;
}

// ── tab ─────────────────────────────────────────────────────────────────────────────────

RibbonTab::RibbonTab(QWidget* parent) : QWidget(parent) {
    row_ = new QHBoxLayout(this);
    row_->setContentsMargins(2, 0, 2, 0);
    row_->setSpacing(0);
    row_->setAlignment(Qt::AlignLeft);
}

RibbonPanel* RibbonTab::addPanel(const QString& title) {
    auto* panel = new RibbonPanel(title, this);
    panel->setObjectName("ribbonPanel");
    row_->addWidget(panel, 0, Qt::AlignTop);

    auto* divider = new QFrame(this);
    divider->setFrameShape(QFrame::VLine);
    divider->setObjectName("ribbonPanelDivider");
    row_->addWidget(divider);
    return panel;
}

// ── ribbon ──────────────────────────────────────────────────────────────────────────────

Ribbon::Ribbon(QWidget* parent) : QWidget(parent) {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    auto* strip = new QWidget(this);
    auto* stripRow = new QHBoxLayout(strip);
    stripRow->setContentsMargins(0, 0, 4, 0);
    stripRow->setSpacing(0);

    tabs_ = new QTabBar(strip);
    tabs_->setExpanding(false);
    tabs_->setDrawBase(false);
    tabs_->setObjectName("ribbonTabs");
    stripRow->addWidget(tabs_, 0);
    stripRow->addStretch(1);

    collapseButton_ = new QToolButton(strip);
    collapseButton_->setText(QStringLiteral("⌃"));   // chevron up
    collapseButton_->setAutoRaise(true);
    collapseButton_->setToolTip(tr("Collapse the ribbon"));
    collapseButton_->setObjectName("ribbonCollapse");
    stripRow->addWidget(collapseButton_, 0);
    outer->addWidget(strip, 0);

    pages_ = new QStackedWidget(this);
    pages_->setObjectName("ribbonPages");
    outer->addWidget(pages_, 0);

    connect(tabs_, &QTabBar::currentChanged, pages_, &QStackedWidget::setCurrentIndex);
    connect(collapseButton_, &QToolButton::clicked, this, [this] { setCollapsed(!collapsed_); });

    setObjectName("ribbon");
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
}

RibbonTab* Ribbon::addTab(const QString& title) {
    auto* tab = new RibbonTab(pages_);
    pages_->addWidget(tab);
    tabs_->addTab(title);
    return tab;
}

void Ribbon::setCurrentTab(int index) { tabs_->setCurrentIndex(index); }

void Ribbon::clearTabs() {
    while (tabs_->count() > 0) tabs_->removeTab(0);
    while (pages_->count() > 0) {
        QWidget* page = pages_->widget(0);
        pages_->removeWidget(page);
        page->deleteLater();   // deferred: a page may be mid-signal when tabs are rebuilt
    }
}

void Ribbon::setCollapsed(bool collapsed) {
    collapsed_ = collapsed;
    pages_->setVisible(!collapsed);
    collapseButton_->setText(collapsed ? QStringLiteral("⌄") : QStringLiteral("⌃"));
    collapseButton_->setToolTip(collapsed ? tr("Expand the ribbon") : tr("Collapse the ribbon"));
}

}  // namespace proshell
