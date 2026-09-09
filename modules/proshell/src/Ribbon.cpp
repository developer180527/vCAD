#include "proshell/Ribbon.h"

#include <QFrame>
#include <QMenu>
#include <QResizeEvent>
#include <QWidgetAction>
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

/// A collapsed panel's stand-in button. Wide enough for a caption like "Primitives" and no wider:
/// the point of collapsing is to give the space back.
constexpr int kCollapsedWidth = 84;

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
    actions_.push_back(action);
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
    actions_.push_back(action);
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

    entries_.push_back(Entry{panel, divider, nullptr, title});
    // A panel added to a tab that is ALREADY at its final size gets no resize to react to, so
    // without this the ribbon stays expanded and overflows until something else resizes it.
    relayout();
    return panel;
}

int RibbonTab::expandedWidth() const {
    int total = row_->contentsMargins().left() + row_->contentsMargins().right();
    for (const Entry& e : entries_) {
        total += e.panel->sizeHint().width() + e.divider->sizeHint().width() + row_->spacing() * 2;
    }
    return total;
}

int RibbonTab::collapsedCount() const {
    int n = 0;
    // `collapsedNow`, NOT isVisible(). Entry::collapsedNow says why at length: an explicitly shown
    // child of a hidden QStackedWidget page misreports, and every tab but the current one IS such a
    // page. Asked about a background tab, isVisible() answered zero however many panels were
    // collapsed -- and this exists to be asked by a test, which is exactly where that would lie.
    for (const Entry& e : entries_) {
        if (e.collapsedNow) ++n;
    }
    return n;
}

void RibbonTab::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    relayout();
}

void RibbonTab::relayout() {
    if (entries_.empty()) return;
    if (laying_) {
        // RECORDED, not dropped. Showing and hiding panels re-lays out this widget, which can
        // deliver another resize before this pass finishes -- and simply returning threw that width
        // away, leaving the ribbon laid out for the previous one until something resized it again.
        pending_ = true;
        return;
    }
    laying_ = true;

    // Until no further resize arrived while we were working. Terminates because each pass reads the
    // current width and the layout it produces cannot change that width.
    do {
        pending_ = false;

    const int available = width();

    // Widths measured from the EXPANDED panel, always -- never from whatever it is showing now.
    // Measuring the current state makes each decision depend on the last, and a panel that
    // collapsed because it was four pixels short then reports the button's width instead and never
    // expands again.
    std::vector<int> widths;
    widths.reserve(entries_.size());
    int total = row_->contentsMargins().left() + row_->contentsMargins().right();
    for (const Entry& e : entries_) {
        const int w = e.panel->sizeHint().width() + e.divider->sizeHint().width()
                      + row_->spacing() * 2;
        widths.push_back(w);
        total += w;
    }

    // Collapse from the RIGHT until it fits. A ribbon's leftmost panels are its primary ones, so
    // the right is where the least is lost -- Office's rule, and Inventor's.
    //
    // NO HYSTERESIS, and that is a conclusion rather than an omission.
    //
    // There was a guard here against the band flickering: collapsing frees space, which lets the
    // panel fit, which expands it, which takes the space back. It could not fire. The loop stops at
    // the FEWEST collapses that fit, so taking one back always needs more room than the width that
    // forced the collapse -- the test reduced to `total_before + stand-in + margin <= available`
    // while the loop had just established `total_before > available`.
    //
    // It could not fire because the flicker it guarded cannot happen either. This decision reads
    // only `width()` -- the tab's own width, which hiding and showing its children does not change
    // -- and starts from every panel expanded each time. Same width in, same layout out, with no
    // memory of the last answer to disagree with. Re-entrancy is what `laying_` handles.
    std::size_t expandedUpTo = entries_.size();
    while (expandedUpTo > 0 && total > available) {
        --expandedUpTo;
        total -= widths[expandedUpTo];
        total += kCollapsedWidth + row_->spacing();   // the stand-in still costs something
    }

    for (std::size_t i = 0; i < entries_.size(); ++i) {
        Entry& e = entries_[i];
        const bool expand = i < expandedUpTo;
        if (expand != e.collapsedNow) continue;   // already in the state we want: touch nothing

        // Built on first use, so a ribbon that is never narrowed pays nothing.
        //
        // The menu lists the panel's own QActions rather than holding the panel widget. Moving the
        // widget into a QWidgetAction reparents it to the menu, and it then never comes back when
        // there is room again -- which is exactly the hang this replaced. Same actions means one
        // enabled state and one place each command lives.
        if (!expand && e.collapsed == nullptr) {
            auto* button = new QToolButton(this);
            button->setText(e.title);
            button->setToolTip(e.title);
            button->setToolButtonStyle(Qt::ToolButtonTextOnly);
            button->setPopupMode(QToolButton::InstantPopup);
            button->setAutoRaise(true);
            // Fixed to the panel's own content height and top-aligned, exactly as a panel is.
            // Left to stretch, the button grows to fill the row, the row grows past the height the
            // ribbon reserved for it, and the band paints over the tab bar above it.
            button->setFixedSize(kCollapsedWidth, kPanelContentHeight);
            button->setObjectName("ribbonCollapsed");

            auto* menu = new QMenu(button);
            for (QAction* action : e.panel->actions()) menu->addAction(action);
            button->setMenu(menu);

            row_->insertWidget(row_->indexOf(e.divider), button, 0, Qt::AlignTop);
            e.collapsed = button;
        }

        e.panel->setVisible(expand);
        e.divider->setVisible(expand);
        if (e.collapsed != nullptr) e.collapsed->setVisible(!expand);
        e.collapsedNow = !expand;
    }
    } while (pending_);

    laying_ = false;
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
