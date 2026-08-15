#pragma once

#include <QIcon>
#include <QString>
#include <QWidget>

#include <functional>
#include <vector>

namespace proshell {

/// A radial right-click menu, as Inventor has.
///
/// Radial rather than linear because DIRECTION is muscle memory. An expert does not read a marking
/// menu — they press, flick, and release, and the flick is the same every time regardless of how
/// long the labels are or how the list was reordered. A linear menu can never offer that, because
/// the position of an item depends on the items above it.
///
/// Behaviour follows the convention the pattern established:
///   * press right button, menu appears centred on the cursor
///   * move outward: the wedge in that direction highlights
///   * release: that wedge fires — or nothing, if the cursor never left the dead zone
///
/// The dead zone matters. Without it, the tiniest movement during a right-click fires a command,
/// and the user who wanted a plain right-click gets a surprise edit.
class MarkingMenu : public QWidget {
    Q_OBJECT
public:
    struct Item {
        QString label;
        QIcon icon;
        std::function<void()> invoke;
        bool enabled = true;
    };

    /// Shows the menu centred at `globalPos` and returns immediately; the chosen item's callback
    /// runs on release. Deletes itself when it closes.
    static void popup(QWidget* parent, const QPoint& globalPos, std::vector<Item> items);

protected:
    void paintEvent(QPaintEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;

private:
    MarkingMenu(QWidget* parent, std::vector<Item> items);
    /// Index of the wedge the cursor points at, or -1 inside the dead zone.
    [[nodiscard]] int wedgeAt(QPointF local) const;

    std::vector<Item> items_;
    int highlighted_ = -1;
};

}  // namespace proshell
