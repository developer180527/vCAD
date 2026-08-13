#include "SketchCanvas.h"

#include "cad/app/Controller.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace cadqt {
namespace {

using cad::sketch::GeoKind;
using cad::sketch::PointRef;

constexpr int kSnapPixels = 10;
/// Tighter than the snap radius: snapping should be eager, selection should not. A pick that grabs
/// the wrong curve is more annoying than one that misses, because the user must notice it first.
constexpr double kPickPixels = 6.0;

const QColor kSelected(0x0a, 0x6c, 0xc4);

/// Distance from a point to a SEGMENT, not to the infinite line. A sketch is full of short
/// collinear segments, and the infinite-line distance would let a click at one end of the sketch
/// select a curve at the other.
double distanceToSegment(QPointF p, QPointF a, QPointF b) {
    const double dx = b.x() - a.x();
    const double dy = b.y() - a.y();
    const double lengthSquared = dx * dx + dy * dy;
    if (lengthSquared < 1e-12) return std::hypot(p.x() - a.x(), p.y() - a.y());
    double t = ((p.x() - a.x()) * dx + (p.y() - a.y()) * dy) / lengthSquared;
    t = std::clamp(t, 0.0, 1.0);
    return std::hypot(p.x() - (a.x() + t * dx), p.y() - (a.y() + t * dy));
}

const QColor kBackground(0xfa, 0xfa, 0xf9);
const QColor kGrid(0xe4, 0xe3, 0xe0);
const QColor kAxis(0xc9, 0xc7, 0xc3);
const QColor kProfile(0x1f, 0x21, 0x24);
const QColor kConstruction(0x9a, 0x9d, 0xa2);
const QColor kPreview(0x0a, 0x6c, 0xc4);
const QColor kSnapMark(0xd8, 0x7a, 0x0a);

}  // namespace

SketchCanvas::SketchCanvas(cad::app::Controller& controller, QWidget* parent)
    : QWidget(parent), controller_(controller) {
    setObjectName(QStringLiteral("sketchCanvas"));
    setAutoFillBackground(true);
    // Mouse tracking so the rubber-band line follows the cursor without a button held: a
    // click-click tool, not click-drag, which is what every CAD sketcher uses.
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::CrossCursor);
}

void SketchCanvas::setTool(Tool t) {
    tool_ = t;
    drawing_ = false;   // switching tools abandons a half-drawn curve
    update();
}

void SketchCanvas::fit() {
    const auto* sketch = controller_.activeSketch();
    origin_ = QPointF(width() * 0.5, height() * 0.5);
    if (sketch == nullptr || sketch->geometry().empty()) {
        scale_ = 3.0;
        update();
        return;
    }

    double minU = 1e30;
    double minV = 1e30;
    double maxU = -1e30;
    double maxV = -1e30;
    const auto grow = [&](double u, double v) {
        minU = std::min(minU, u);
        maxU = std::max(maxU, u);
        minV = std::min(minV, v);
        maxV = std::max(maxV, v);
    };
    for (const auto& g : sketch->geometry()) {
        switch (g.kind) {
            case GeoKind::Line:
                grow(g.p[0], g.p[1]);
                grow(g.p[2], g.p[3]);
                break;
            case GeoKind::Circle:
            case GeoKind::Arc:
                grow(g.p[0] - g.p[2], g.p[1] - g.p[2]);
                grow(g.p[0] + g.p[2], g.p[1] + g.p[2]);
                break;
            case GeoKind::Point:
                grow(g.p[0], g.p[1]);
                break;
        }
    }
    const double spanU = std::max(maxU - minU, 1.0);
    const double spanV = std::max(maxV - minV, 1.0);
    // 0.85 leaves a margin, so geometry never touches the frame and dimensions have somewhere to go.
    scale_ = 0.85 * std::min(width() / spanU, height() / spanV);
    origin_ = QPointF(width() * 0.5 - (minU + maxU) * 0.5 * scale_,
                      height() * 0.5 + (minV + maxV) * 0.5 * scale_);
    update();
}

QPointF SketchCanvas::toScreen(double u, double v) const {
    // v is NEGATED: sketch Y points up, screen Y points down. Forgetting this draws a mirrored
    // profile that looks plausible until a dimension disagrees with it.
    return QPointF(origin_.x() + u * scale_, origin_.y() - v * scale_);
}

std::pair<double, double> SketchCanvas::toSketch(QPointF p) const {
    return {(p.x() - origin_.x()) / scale_, (origin_.y() - p.y()) / scale_};
}

SketchCanvas::Snap SketchCanvas::snapAt(QPointF screen) const {
    Snap best;
    const auto* sketch = controller_.activeSketch();
    if (sketch == nullptr) return best;

    double bestDistance = kSnapPixels;
    const auto& ids = sketch->ids();
    const auto& geometry = sketch->geometry();
    for (std::size_t i = 0; i < geometry.size() && i < ids.size(); ++i) {
        for (const PointRef ref : {PointRef::Start, PointRef::End, PointRef::Center}) {
            const auto point = sketch->pointAt(ids[i], ref);
            if (!point) continue;
            const QPointF at = toScreen(point.value()[0], point.value()[1]);
            const double d = std::hypot(at.x() - screen.x(), at.y() - screen.y());
            if (d < bestDistance) {
                bestDistance = d;
                best = {true, ids[i], static_cast<int>(ref), point.value()[0], point.value()[1]};
            }
        }
    }
    return best;
}

std::uint32_t SketchCanvas::pickAt(QPointF screen) const {
    const auto* sketch = controller_.activeSketch();
    if (sketch == nullptr) return cad::sketch::kNoGeo;

    std::uint32_t best = cad::sketch::kNoGeo;
    double bestDistance = kPickPixels;
    const auto& ids = sketch->ids();
    const auto& geometry = sketch->geometry();

    for (std::size_t i = 0; i < geometry.size() && i < ids.size(); ++i) {
        const auto& g = geometry[i];
        double d = 1e30;
        switch (g.kind) {
            case GeoKind::Line:
                d = distanceToSegment(screen, toScreen(g.p[0], g.p[1]), toScreen(g.p[2], g.p[3]));
                break;
            case GeoKind::Circle: {
                const QPointF c = toScreen(g.p[0], g.p[1]);
                // Distance to the RIM, not to the centre: a circle is the curve, and picking its
                // middle would select it from anywhere inside a large one.
                d = std::abs(std::hypot(screen.x() - c.x(), screen.y() - c.y()) - g.p[2] * scale_);
                break;
            }
            case GeoKind::Arc: {
                const QPointF c = toScreen(g.p[0], g.p[1]);
                const double radial =
                    std::abs(std::hypot(screen.x() - c.x(), screen.y() - c.y()) - g.p[2] * scale_);
                // Only counts if the cursor is within the arc's SWEEP. Without this an arc is
                // pickable all the way round the circle it belongs to, including the part that is
                // not drawn.
                double angle = std::atan2(-(screen.y() - c.y()), screen.x() - c.x());
                double start = g.p[3];
                double end = g.p[4];
                while (end < start) end += 2 * M_PI;
                while (angle < start) angle += 2 * M_PI;
                d = (angle <= end) ? radial : 1e30;
                break;
            }
            case GeoKind::Point: {
                const QPointF at = toScreen(g.p[0], g.p[1]);
                d = std::hypot(screen.x() - at.x(), screen.y() - at.y());
                break;
            }
        }
        if (d < bestDistance) {
            bestDistance = d;
            best = ids[i];
        }
    }
    return best;
}

void SketchCanvas::paintEvent(QPaintEvent*) {
    QPainter g(this);
    g.setRenderHint(QPainter::Antialiasing, true);
    g.fillRect(rect(), kBackground);

    // Grid at a round sketch-unit spacing that stays legible as you zoom: step up by decades so the
    // lines never crowd into a solid block.
    double step = 10.0;
    while (step * scale_ < 8.0) step *= 10.0;
    while (step * scale_ > 80.0) step /= 10.0;
    g.setPen(QPen(kGrid, 1.0));
    const auto [leftU, topV] = toSketch(QPointF(0, 0));
    const auto [rightU, bottomV] = toSketch(QPointF(width(), height()));
    for (double u = std::floor(leftU / step) * step; u <= rightU; u += step) {
        const double x = toScreen(u, 0).x();
        g.drawLine(QPointF(x, 0), QPointF(x, height()));
    }
    for (double v = std::floor(bottomV / step) * step; v <= topV; v += step) {
        const double y = toScreen(0, v).y();
        g.drawLine(QPointF(0, y), QPointF(width(), y));
    }
    g.setPen(QPen(kAxis, 1.4));
    g.drawLine(QPointF(0, origin_.y()), QPointF(width(), origin_.y()));
    g.drawLine(QPointF(origin_.x(), 0), QPointF(origin_.x(), height()));

    const auto* sketch = controller_.activeSketch();
    if (sketch == nullptr) return;

    const auto& selection = controller_.sketchSelection();
    const auto& geoIds = sketch->ids();
    for (std::size_t gi = 0; gi < sketch->geometry().size(); ++gi) {
        const auto& geo = sketch->geometry()[gi];
        const bool isSelected =
            gi < geoIds.size()
            && std::find(selection.begin(), selection.end(), geoIds[gi]) != selection.end();
        QPen pen(isSelected ? kSelected : (geo.construction ? kConstruction : kProfile),
                 isSelected ? 2.8 : (geo.construction ? 1.2 : 2.0));
        if (geo.construction) pen.setStyle(Qt::DashLine);
        g.setPen(pen);
        switch (geo.kind) {
            case GeoKind::Line:
                g.drawLine(toScreen(geo.p[0], geo.p[1]), toScreen(geo.p[2], geo.p[3]));
                break;
            case GeoKind::Circle: {
                const QPointF c = toScreen(geo.p[0], geo.p[1]);
                const double r = geo.p[2] * scale_;
                g.drawEllipse(c, r, r);
                break;
            }
            case GeoKind::Arc: {
                const QPointF c = toScreen(geo.p[0], geo.p[1]);
                const double r = geo.p[2] * scale_;
                const QRectF box(c.x() - r, c.y() - r, r * 2, r * 2);
                // Qt angles are 1/16th degrees and counter-clockwise from 3 o'clock, which matches
                // our convention once the screen's Y flip is accounted for by negating.
                const double startDeg = geo.p[3] * 180.0 / M_PI;
                const double endDeg = geo.p[4] * 180.0 / M_PI;
                g.drawArc(box, static_cast<int>(startDeg * 16),
                          static_cast<int>((endDeg - startDeg) * 16));
                break;
            }
            case GeoKind::Point:
                g.setBrush(kProfile);
                g.drawEllipse(toScreen(geo.p[0], geo.p[1]), 2.5, 2.5);
                g.setBrush(Qt::NoBrush);
                break;
        }
    }

    // Rubber band for the curve being drawn, plus the snap marker. Both are feedback the user needs
    // BEFORE committing a click: a snap they cannot see is a snap they cannot rely on.
    if (drawing_) {
        g.setPen(QPen(kPreview, 1.6, Qt::DashLine));
        const QPointF from = toScreen(startU_, startV_);
        if (tool_ == Tool::Circle) {
            const double r = std::hypot(cursor_.x() - from.x(), cursor_.y() - from.y());
            g.drawEllipse(from, r, r);
        } else {
            g.drawLine(from, cursor_);
        }
    }
    if (const Snap snap = snapAt(cursor_); snap.found) {
        g.setPen(QPen(kSnapMark, 1.8));
        const QPointF at = toScreen(snap.u, snap.v);
        g.drawRect(QRectF(at.x() - 4, at.y() - 4, 8, 8));
    }
}

void SketchCanvas::mousePressEvent(QMouseEvent* event) {
    auto* sketch = controller_.activeSketch();
    if (sketch == nullptr) return;

    if (tool_ == Tool::Select) {
        if (event->button() != Qt::LeftButton) return;
        const std::uint32_t hit = pickAt(event->position());
        if (hit == cad::sketch::kNoGeo) {
            // Clicking empty space clears, which is what every editor does and what makes a
            // stale selection impossible to leave behind by accident.
            controller_.clearSketchSelection();
        } else {
            const bool additive = (event->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier))
                                  != 0;
            controller_.selectSketchGeometry(hit, additive);
        }
        emit sketchChanged();
        update();
        return;
    }
    if (event->button() == Qt::RightButton) {
        drawing_ = false;   // right-click abandons, as it does everywhere
        update();
        return;
    }
    if (event->button() != Qt::LeftButton) return;

    const Snap snap = snapAt(event->position());
    const auto [u, v] = snap.found ? std::pair{snap.u, snap.v} : toSketch(event->position());

    if (!drawing_) {
        drawing_ = true;
        startU_ = u;
        startV_ = v;
        startSnap_ = snap;
        cursor_ = event->position();
        update();
        return;
    }

    if (tool_ == Tool::Circle) {
        const double radius = std::hypot(u - startU_, v - startV_);
        if (radius > 1e-9) sketch->addCircle(startU_, startV_, radius);
    } else {
        const auto line = sketch->addLine(startU_, startV_, u, v);
        // Snapped endpoints become real CONSTRAINTS, not just matching coordinates. Coordinates
        // drift the moment anything else solves; a coincidence does not, and it is what makes the
        // profile stay closed while the user keeps editing.
        if (startSnap_.found) {
            sketch->coincident(line, PointRef::Start, startSnap_.geo,
                               static_cast<PointRef>(startSnap_.point));
        }
        if (snap.found) {
            sketch->coincident(line, PointRef::End, snap.geo,
                               static_cast<PointRef>(snap.point));
        }
        // Chained drawing: the next segment starts where this one ended, which is how a polyline is
        // drawn in every CAD application. Ending on a snap closes the loop and stops the chain.
        if (snap.found) {
            drawing_ = false;
        } else {
            startU_ = u;
            startV_ = v;
            startSnap_ = Snap{true, line, static_cast<int>(PointRef::End), u, v};
        }
    }

    if (tool_ == Tool::Circle) drawing_ = false;
    controller_.solveSketch();
    emit sketchChanged();
    update();
}

void SketchCanvas::mouseMoveEvent(QMouseEvent* event) {
    cursor_ = event->position();
    update();
}

void SketchCanvas::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        if (drawing_) {
            drawing_ = false;   // first Escape abandons the curve, not the sketch
            update();
        } else {
            controller_.finishSketch();
        }
        return;
    }
    if (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) {
        controller_.deleteSketchSelection();
        emit sketchChanged();
        update();
        return;
    }
    if (event->key() == Qt::Key_L) setTool(Tool::Line);
    else if (event->key() == Qt::Key_C) setTool(Tool::Circle);
    else if (event->key() == Qt::Key_S) setTool(Tool::Select);
    else QWidget::keyPressEvent(event);
}

void SketchCanvas::wheelEvent(QWheelEvent* event) {
    // Zoom about the CURSOR, not the centre: zooming toward a corner is the whole reason to zoom,
    // and centre-anchored zoom means panning after every wheel tick.
    const double factor = event->angleDelta().y() > 0 ? 1.15 : 1.0 / 1.15;
    const auto before = toSketch(event->position());
    scale_ = std::clamp(scale_ * factor, 0.05, 5000.0);
    const QPointF after = toScreen(before.first, before.second);
    origin_ += event->position() - after;
    update();
}

void SketchCanvas::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    update();
}

}  // namespace cadqt
