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

private:
    QHBoxLayout* row_;
    QWidget* currentSmallColumn_ = nullptr;
    int smallInColumn_ = 0;
};

class RibbonTab : public QWidget {
    Q_OBJECT
public:
    explicit RibbonTab(QWidget* parent = nullptr);
    RibbonPanel* addPanel(const QString& title);

private:
    QHBoxLayout* row_;
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
