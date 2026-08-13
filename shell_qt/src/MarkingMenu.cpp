#include "MarkingMenu.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include <cmath>
#include <numbers>

namespace cadqt {
namespace {

constexpr int kRadius = 96;        ///< outer radius in pixels
constexpr int kDeadZone = 22;      ///< no selection inside this; see the header
constexpr int kLabelRadius = 66;

const QColor kFill(0xf6, 0xf5, 0xf3, 0xf2);
const QColor kEdge(0xb8, 0xb6, 0xb1);
const QColor kHot(0x0a, 0x6c, 0xc4);
const QColor kText(0x1f, 0x21, 0x24);
const QColor kDisabled(0xa8, 0xab, 0xaf);

}  // namespace

MarkingMenu::MarkingMenu(QWidget* parent, std::vector<Item> items)
    : QWidget(parent, Qt::Popup | Qt::FramelessWindowHint), items_(std::move(items)) {
    setAttribute(Qt::WA_DeleteOnClose);
    setAttribute(Qt::WA_TranslucentBackground);
    setMouseTracking(true);
    setFixedSize(kRadius * 2 + 2, kRadius * 2 + 2);
}

void MarkingMenu::popup(QWidget* parent, const QPoint& globalPos, std::vector<Item> items) {
    if (items.empty()) return;
    auto* menu = new MarkingMenu(parent, std::move(items));
    // Centred ON the cursor, not below-right of it. The whole point is that every item is the same
    // small distance away in a different direction.
    menu->move(globalPos - QPoint(menu->width() / 2, menu->height() / 2));
    menu->show();
    menu->setFocus();
}

int MarkingMenu::wedgeAt(QPointF local) const {
    const QPointF centre(width() / 2.0, height() / 2.0);
    const double dx = local.x() - centre.x();
    const double dy = local.y() - centre.y();
    if (std::hypot(dx, dy) < kDeadZone) return -1;

    // Angle measured clockwise from straight up, so wedge 0 is at 12 o'clock. Up first because it
    // is the easiest direction to hit reliably and should carry the most common command.
    double angle = std::atan2(dx, -dy);
    if (angle < 0) angle += 2 * std::numbers::pi;
    const double span = 2 * std::numbers::pi / static_cast<double>(items_.size());
    return static_cast<int>(angle / span) % static_cast<int>(items_.size());
}

void MarkingMenu::paintEvent(QPaintEvent*) {
    QPainter g(this);
    g.setRenderHint(QPainter::Antialiasing, true);
    const QPointF centre(width() / 2.0, height() / 2.0);
    const int count = static_cast<int>(items_.size());
    const double span = 360.0 / count;

    for (int i = 0; i < count; ++i) {
        // Qt arcs run counter-clockwise from 3 o'clock in 1/16 degrees; our wedges run clockwise
        // from 12. Converting here rather than changing wedgeAt keeps the hit test readable, which
        // is the half that has to be right.
        const double startDeg = 90.0 - (i + 1) * span;
        QPainterPath wedge;
        wedge.moveTo(centre);
        wedge.arcTo(QRectF(centre.x() - kRadius, centre.y() - kRadius, kRadius * 2, kRadius * 2),
                    startDeg, span);
        wedge.closeSubpath();

        const bool hot = i == highlighted_ && items_[i].enabled;
        g.setBrush(hot ? QColor(0xd7, 0xe5, 0xf3, 0xf8) : kFill);
        g.setPen(QPen(hot ? kHot : kEdge, hot ? 1.6 : 1.0));
        g.drawPath(wedge);

        const double mid = (90.0 - (i + 0.5) * span) * std::numbers::pi / 180.0;
        const QPointF at(centre.x() + std::cos(mid) * kLabelRadius,
                         centre.y() - std::sin(mid) * kLabelRadius);
        g.setPen(items_[i].enabled ? kText : kDisabled);
        const QRectF box(at.x() - 44, at.y() - 12, 88, 24);
        g.drawText(box, Qt::AlignCenter | Qt::TextWordWrap, items_[i].label);
    }

    // The dead zone is drawn, so its size is discoverable rather than something the user has to
    // infer from commands not firing.
    g.setBrush(kFill);
    g.setPen(QPen(kEdge, 1.0));
    g.drawEllipse(centre, kDeadZone, kDeadZone);
}

void MarkingMenu::mouseMoveEvent(QMouseEvent* event) {
    const int wedge = wedgeAt(event->position());
    if (wedge != highlighted_) {
        highlighted_ = wedge;
        update();
    }
}

void MarkingMenu::mouseReleaseEvent(QMouseEvent* event) {
    const int wedge = wedgeAt(event->position());
    // Copied out before close(): WA_DeleteOnClose destroys this object, and invoking a callback
    // that lives in a destroyed vector is a use-after-free that only shows up under load.
    std::function<void()> chosen;
    if (wedge >= 0 && wedge < static_cast<int>(items_.size()) && items_[wedge].enabled) {
        chosen = items_[wedge].invoke;
    }
    close();
    if (chosen) chosen();
}

void MarkingMenu::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) close();
    else QWidget::keyPressEvent(event);
}

}  // namespace cadqt
