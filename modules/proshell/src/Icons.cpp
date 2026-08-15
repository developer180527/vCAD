#include "proshell/Icons.h"

#include <QGuiApplication>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>

#include <algorithm>

namespace proshell {
namespace {

// Dark strokes: the default chrome is light (Inventor's default), and light icons were invisible
// on it. An application with a dark theme calls IconSet::setColours rather than editing this.
constexpr QColor kLine(0x3c, 0x40, 0x45);
constexpr QColor kAccent(0x0a, 0x6c, 0xc4);

/// A circular arrow, the universal undo/redo glyph.
void drawArrow(QPainter& g, int s, bool clockwise) {
    const QRectF r(s * 0.22, s * 0.24, s * 0.56, s * 0.52);
    g.drawArc(r, clockwise ? 30 * 16 : 150 * 16, clockwise ? 240 * 16 : -240 * 16);
    const QPointF tip = clockwise ? QPointF(s * 0.75, s * 0.32) : QPointF(s * 0.25, s * 0.32);
    const qreal a = s * 0.13;
    QPainterPath head;
    head.moveTo(tip);
    head.lineTo(tip + QPointF(clockwise ? -a : a, 0));
    head.lineTo(tip + QPointF(0, a));
    head.closeSubpath();
    g.fillPath(head, g.pen().color());
}

/// The glyphs every professional application shares.
///
/// The bar for inclusion is that the glyph means the same thing in a CAD app, a word processor and
/// a video editor. `save` passes. `fit` — zoom to fit — does not, because it presupposes a
/// viewport, and an application without one would carry a glyph it can never use.
bool paintUniversal(QPainter& g, const QString& name, int size) {
    const QPen pen = g.pen();
    const auto accent = [&] { g.setPen(QPen(IconSet::instance().accent(), pen.widthF())); };

    if (name == "new" || name == "open" || name == "save") {
        g.drawRect(QRectF(size * 0.24, size * 0.18, size * 0.48, size * 0.64));
        if (name == "save") g.drawRect(QRectF(size * 0.34, size * 0.50, size * 0.28, size * 0.32));
        if (name == "open") {
            g.drawLine(QPointF(size * 0.24, size * 0.34), QPointF(size * 0.72, size * 0.34));
        }
        return true;
    }
    if (name == "undo") {
        drawArrow(g, size, false);
        return true;
    }
    if (name == "redo") {
        drawArrow(g, size, true);
        return true;
    }
    if (name == "delete") {
        g.drawLine(QPointF(size * 0.28, size * 0.28), QPointF(size * 0.72, size * 0.72));
        g.drawLine(QPointF(size * 0.72, size * 0.28), QPointF(size * 0.28, size * 0.72));
        return true;
    }
    if (name == "import") {
        // A document with an arrow going into it. Paired with the file glyph above so the two
        // read as the same family.
        g.drawRect(QRectF(size * 0.40, size * 0.18, size * 0.42, size * 0.64));
        accent();
        g.drawLine(QPointF(size * 0.16, size * 0.50), QPointF(size * 0.46, size * 0.50));
        g.drawLine(QPointF(size * 0.46, size * 0.50), QPointF(size * 0.34, size * 0.40));
        g.drawLine(QPointF(size * 0.46, size * 0.50), QPointF(size * 0.34, size * 0.60));
        return true;
    }
    if (name == "settings") {
        for (int r = 0; r < 3; ++r) {
            const qreal y = size * (0.30 + r * 0.18);
            g.drawLine(QPointF(size * 0.20, y), QPointF(size * 0.80, y));
        }
        accent();
        g.drawEllipse(QRectF(size * 0.54, size * 0.24, size * 0.12, size * 0.12));
        g.drawEllipse(QRectF(size * 0.28, size * 0.42, size * 0.12, size * 0.12));
        return true;
    }
    if (name == "view-list" || name == "view-grid") {
        if (name == "view-list") {
            for (int r = 0; r < 3; ++r) {
                const qreal y = size * (0.28 + r * 0.20);
                g.drawLine(QPointF(size * 0.22, y), QPointF(size * 0.78, y));
            }
        } else {
            for (int r = 0; r < 2; ++r) {
                for (int c = 0; c < 2; ++c) {
                    g.drawRect(QRectF(size * (0.22 + c * 0.30), size * (0.22 + r * 0.30),
                                      size * 0.26, size * 0.26));
                }
            }
        }
        return true;
    }
    return false;
}

}  // namespace

IconSet::IconSet() : line_(kLine), accent_(kAccent) {
    providers_.emplace_back(paintUniversal);
}

IconSet& IconSet::instance() {
    // Function-local static: initialised on first use, which matters because applications register
    // their provider from a startup function whose order relative to this library's own
    // initialisation is not something either side should have to reason about.
    static IconSet set;
    return set;
}

void IconSet::addProvider(GlyphPainter painter) {
    if (painter) providers_.push_back(std::move(painter));
}

void IconSet::setColours(QColor line, QColor accent) noexcept {
    line_ = line;
    accent_ = accent;
}

QIcon IconSet::icon(const QString& name, int size) const {
    // Allocate in DEVICE pixels, then tell Qt the ratio so it treats the pixmap as `size` logical
    // points. Allocating `size x size` on a 2x display gives Qt half the pixels it needs and it
    // upscales — which is exactly what "the buttons look blurry" was.
    //
    // The painter still works in logical coordinates after setDevicePixelRatio, so every glyph
    // body is unchanged by this; they just land on a finer grid.
    const qreal dpr = qApp ? qApp->devicePixelRatio() : 1.0;
    QPixmap pm(QSize(size, size) * dpr);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);

    QPainter g(&pm);
    g.setRenderHint(QPainter::Antialiasing, true);
    QPen pen(line_);
    pen.setWidthF(std::max(1.2, size / 20.0));
    pen.setJoinStyle(Qt::MiterJoin);
    g.setPen(pen);

    // Most recently registered first, so an application overrides a built-in by registering after
    // this library rather than by patching it.
    bool drawn = false;
    for (auto it = providers_.rbegin(); it != providers_.rend() && !drawn; ++it) {
        // The pen is restored between providers: one that draws partially before deciding the name
        // is not its own would otherwise leave the next provider with the wrong colour.
        g.setPen(pen);
        drawn = (*it)(g, name, size);
    }
    if (!drawn) {
        g.setPen(pen);
        g.drawRect(QRectF(size * 0.28, size * 0.28, size * 0.44, size * 0.44));
    }

    g.end();
    return QIcon(pm);
}

}  // namespace proshell
