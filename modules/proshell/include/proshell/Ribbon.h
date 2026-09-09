#pragma once

// A ribbon, because Qt has no ribbon and toolbars are why FreeCAD looks like 2006 (ADR 0008).
//
// Structure, matching Inventor:
//
//   ┌─ tabs ────────────────────────────────────────────────┐
//   │ 3D Model │ Sketch │ View │ Manage                     │
//   ├───────────────────────────────────────────────────────┤
//   │ ┌ Primitives ─┐ ┌ Modify ──┐ ┌ Pattern ─┐             │  <- panels
//   │ │ [Box] [Cyl] │ │ [Fillet] │ │ ...      │             │  <- large tool buttons
//   │ └─────────────┘ └──────────┘ └──────────┘             │
//   └───────────────────────────────────────────────────────┘
//
// Panels carry a caption underneath, which is the detail that reads as "ribbon" rather than
// "toolbar in a tab widget".

#include <QAction>
#include <QHBoxLayout>
#include <QStackedWidget>
#include <QString>
#include <QTabBar>
#include <QToolButton>
#include <QWidget>

#include <vector>

namespace proshell {

/// A group of related commands with a caption. `Panel` rather than `Group` to match Inventor's
/// own terminology, so screenshots and docs line up.
class RibbonPanel : public QWidget {
    Q_OBJECT
public:
    explicit RibbonPanel(const QString& title, QWidget* parent = nullptr);

    /// Large button: icon above label, the ribbon's primary affordance.
    QToolButton* addLarge(QAction*);
    /// Small button: icon beside label, for secondary commands stacked three per column.
    QToolButton* addSmall(QAction*);
    void addSeparator();

    /// Every action this panel shows, in the order it shows them.
    ///
    /// Kept so a collapsed panel can offer the SAME QActions in a menu rather than a second set of
    /// buttons. One action means one enabled state and one place the command lives: a duplicate
    /// would drift, and the copy in the popup would stop greying out with the original.
    [[nodiscard]] const std::vector<QAction*>& actions() const noexcept { return actions_; }

private:
    /// Drops the cached size hint after a button is added. See the definition: the cache is dropped
    /// by an event otherwise, and the ribbon reads the hint before that event arrives.
    void invalidateHint();

    QHBoxLayout* row_;
    QWidget* currentSmallColumn_ = nullptr;
    int smallInColumn_ = 0;
    std::vector<QAction*> actions_;
};

/// A tab's row of panels, which COLLAPSES rather than squeezes when the window is too narrow.
///
/// # Why this is not just a layout
///
/// A QHBoxLayout given less room than its children need shrinks them below their size hint, and a
/// QToolButton that is too narrow elides its label. At 900 px the ribbon read "Sta...tch",
/// "ExtrudeRevolve" run together, and small buttons showing a bare "..." -- every command still
/// present and none of them legible. A window spends real time at that width.
///
/// Inventor and Office both answer this the same way: when the panels no longer fit, the ones on
/// the RIGHT collapse into a single button that opens the panel as a popup. Nothing is removed and
/// nothing is squeezed; the panel simply stops being spread out. That is what this does, and it is
/// copied deliberately rather than invented -- a user who knows either application already knows
/// what the button means.
///
/// Right-to-left because a ribbon puts its primary commands on the left, so collapsing from the
/// right takes the least-used panels first. Same rule as Office.
class RibbonTab : public QWidget {
    Q_OBJECT
public:
    explicit RibbonTab(QWidget* parent = nullptr);
    RibbonPanel* addPanel(const QString& title);

    /// How wide this tab would like to be with every panel expanded. Public so a test can ask,
    /// because "does it collapse" is otherwise a question only a screenshot can answer.
    [[nodiscard]] int expandedWidth() const;
    /// How many panels are currently collapsed into popup buttons.
    [[nodiscard]] int collapsedCount() const;

protected:
    void resizeEvent(QResizeEvent*) override;

private:
    /// One panel, and the button that stands in for it when there is no room.
    struct Entry {
        RibbonPanel* panel = nullptr;
        QWidget* divider = nullptr;
        QToolButton* collapsed = nullptr;   ///< created lazily, hidden while the panel is shown
        QString title;
        /// What we last asked for, so visibility is only ever CHANGED, never re-asserted.
        ///
        /// Qt marks a widget explicitly shown the moment show() is called on it, and an explicitly
        /// shown child of a QStackedWidget page ignores the page being hidden -- so every tab's
        /// panels paint at once, over the tab bar. Re-asserting "visible" on an already visible
        /// panel is exactly that call, which is why this is tracked here rather than read back from
        /// isVisible().
        bool collapsedNow = false;
    };

    void relayout();

    QHBoxLayout* row_;
    std::vector<Entry> entries_;
    /// Guards against the relayout that a relayout causes. Showing or hiding a child changes this
    /// widget's layout, which can deliver another resize before the first has finished.
    bool laying_ = false;
    /// Set when a resize arrives during a relayout, so the pass repeats at the newer width instead
    /// of the layout settling on the older one.
    bool pending_ = false;
};

class Ribbon : public QWidget {
    Q_OBJECT
public:
    explicit Ribbon(QWidget* parent = nullptr);

    RibbonTab* addTab(const QString& title);
    void setCurrentTab(int index);

    /// Removes every tab. Ribbon tabs are derived from the active workspace rather than
    /// registered once (ADR 0009), so this runs on every document switch.
    void clearTabs();

    /// Collapse to just the tab strip. Inventor has this and users of small laptops rely on it;
    /// a ribbon that cannot get out of the way is a ribbon people resent.
    void setCollapsed(bool);
    [[nodiscard]] bool collapsed() const noexcept { return collapsed_; }

private:
    QTabBar* tabs_;
    QStackedWidget* pages_;
    QToolButton* collapseButton_;
    bool collapsed_ = false;
};

}  // namespace proshell
