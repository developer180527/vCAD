#include "Icons.h"

#include <QGuiApplication>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>

namespace cadqt {
namespace {

// Dark strokes: the chrome is light now (Inventor's default), so light icons were
// invisible on it — a straight consequence of the theme change.
constexpr QColor kLine(0x3c, 0x40, 0x45);
constexpr QColor kAccent(0x0a, 0x6c, 0xc4);

void drawBox(QPainter& g, int s) {
    const qreal d = s * 0.22;   // isometric depth offset
    const QRectF front(s * 0.16, s * 0.30, s * 0.52, s * 0.52);
    g.drawRect(front);
    g.drawLine(front.topLeft(), front.topLeft() + QPointF(d, -d));
    g.drawLine(front.topRight(), front.topRight() + QPointF(d, -d));
    g.drawLine(front.bottomRight(), front.bottomRight() + QPointF(d, -d));
    g.drawLine(front.topLeft() + QPointF(d, -d), front.topRight() + QPointF(d, -d));
    g.drawLine(front.topRight() + QPointF(d, -d), front.bottomRight() + QPointF(d, -d));
}

void drawCylinder(QPainter& g, int s) {
    const QRectF top(s * 0.22, s * 0.20, s * 0.56, s * 0.20);
    g.drawEllipse(top);
    g.drawLine(QPointF(s * 0.22, s * 0.30), QPointF(s * 0.22, s * 0.70));
    g.drawLine(QPointF(s * 0.78, s * 0.30), QPointF(s * 0.78, s * 0.70));
    g.drawArc(QRectF(s * 0.22, s * 0.60, s * 0.56, s * 0.20), 180 * 16, 180 * 16);
}

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

}  // namespace

QIcon icon(const QString& name, int size) {
    // Allocate in DEVICE pixels, then tell Qt the ratio so it treats the pixmap as `size`
    // logical points. Allocating `size x size` on a 2x display gives Qt half the pixels it
    // needs and it upscales — which is exactly what "the buttons look blurry" was.
    //
    // The painter still works in logical coordinates after setDevicePixelRatio, so every
    // draw* helper below is unchanged; they just land on a finer grid.
    const qreal dpr = qApp ? qApp->devicePixelRatio() : 1.0;
    QPixmap pm(QSize(size, size) * dpr);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter g(&pm);
    g.setRenderHint(QPainter::Antialiasing, true);
    QPen pen(kLine);
    pen.setWidthF(std::max(1.2, size / 20.0));
    pen.setJoinStyle(Qt::MiterJoin);
    g.setPen(pen);

    if (name == "box") {
        drawBox(g, size);
    } else if (name == "cylinder") {
        drawCylinder(g, size);
    } else if (name == "cut") {
        drawBox(g, size);
        g.setPen(QPen(kAccent, pen.widthF()));
        g.drawEllipse(QRectF(size * 0.42, size * 0.44, size * 0.40, size * 0.40));
    } else if (name == "undo") {
        drawArrow(g, size, false);
    } else if (name == "redo") {
        drawArrow(g, size, true);
    } else if (name == "delete") {
        g.drawLine(QPointF(size * 0.28, size * 0.28), QPointF(size * 0.72, size * 0.72));
        g.drawLine(QPointF(size * 0.72, size * 0.28), QPointF(size * 0.28, size * 0.72));
    } else if (name == "fit") {
        const QRectF r(size * 0.24, size * 0.24, size * 0.52, size * 0.52);
        g.drawRect(r);
        g.setPen(QPen(kAccent, pen.widthF()));
        g.drawLine(r.center() - QPointF(size * 0.08, 0), r.center() + QPointF(size * 0.08, 0));
        g.drawLine(r.center() - QPointF(0, size * 0.08), r.center() + QPointF(0, size * 0.08));
    } else if (name == "ortho") {
        g.drawRect(QRectF(size * 0.24, size * 0.32, size * 0.36, size * 0.36));
        g.drawRect(QRectF(size * 0.40, size * 0.24, size * 0.36, size * 0.36));
    } else if (name == "part") {
        drawBox(g, size);
    } else if (name == "assembly") {
        // Two blocks, one offset: the universal "this is made of other things" glyph.
        g.drawRect(QRectF(size * 0.14, size * 0.36, size * 0.40, size * 0.40));
        g.setPen(QPen(kAccent, pen.widthF()));
        g.drawRect(QRectF(size * 0.44, size * 0.20, size * 0.40, size * 0.40));
    } else if (name == "drawing") {
        g.drawRect(QRectF(size * 0.20, size * 0.14, size * 0.60, size * 0.72));
        g.setPen(QPen(kAccent, pen.widthF()));
        g.drawLine(QPointF(size * 0.28, size * 0.70), QPointF(size * 0.72, size * 0.70));
        g.drawRect(QRectF(size * 0.30, size * 0.26, size * 0.34, size * 0.30));
    } else if (name == "presentation") {
        g.drawRect(QRectF(size * 0.16, size * 0.40, size * 0.30, size * 0.30));
        g.setPen(QPen(kAccent, pen.widthF()));
        g.drawLine(QPointF(size * 0.52, size * 0.55), QPointF(size * 0.82, size * 0.55));
        g.drawLine(QPointF(size * 0.74, size * 0.47), QPointF(size * 0.82, size * 0.55));
        g.drawLine(QPointF(size * 0.74, size * 0.63), QPointF(size * 0.82, size * 0.55));
    } else if (name == "new" || name == "open" || name == "save") {
        g.drawRect(QRectF(size * 0.24, size * 0.18, size * 0.48, size * 0.64));
        if (name == "save") g.drawRect(QRectF(size * 0.34, size * 0.50, size * 0.28, size * 0.32));
        if (name == "open") g.drawLine(QPointF(size * 0.24, size * 0.34),
                                       QPointF(size * 0.72, size * 0.34));
    } else if (name == "sketch" || name == "sketch-edit") {
        // A sketch plane with a profile on it.
        g.drawRect(QRectF(size * 0.16, size * 0.30, size * 0.68, size * 0.44));
        g.setPen(QPen(kAccent, pen.widthF()));
        g.drawLine(QPointF(size * 0.30, size * 0.62), QPointF(size * 0.44, size * 0.40));
        g.drawLine(QPointF(size * 0.44, size * 0.40), QPointF(size * 0.68, size * 0.56));
        if (name == "sketch-edit") {
            g.drawLine(QPointF(size * 0.60, size * 0.78), QPointF(size * 0.82, size * 0.56));
        }
    } else if (name == "extrude") {
        // A profile pushed along an axis: the defining gesture of the command.
        g.drawRect(QRectF(size * 0.18, size * 0.52, size * 0.32, size * 0.28));
        g.setPen(QPen(kAccent, pen.widthF()));
        g.drawLine(QPointF(size * 0.52, size * 0.60), QPointF(size * 0.82, size * 0.30));
        g.drawLine(QPointF(size * 0.82, size * 0.30), QPointF(size * 0.70, size * 0.32));
        g.drawLine(QPointF(size * 0.82, size * 0.30), QPointF(size * 0.80, size * 0.42));
    } else if (name == "revolve") {
        g.drawLine(QPointF(size * 0.24, size * 0.16), QPointF(size * 0.24, size * 0.84));
        g.setPen(QPen(kAccent, pen.widthF()));
        g.drawEllipse(QRectF(size * 0.34, size * 0.28, size * 0.48, size * 0.44));
    } else if (name == "fillet" || name == "chamfer") {
        // The same corner, rounded or cut. Drawn as a pair so they read as siblings.
        QPainterPath path;
        path.moveTo(size * 0.22, size * 0.80);
        path.lineTo(size * 0.22, size * 0.42);
        if (name == "fillet") {
            path.quadTo(size * 0.22, size * 0.22, size * 0.60, size * 0.22);
        } else {
            path.lineTo(size * 0.42, size * 0.22);
            path.lineTo(size * 0.60, size * 0.22);
        }
        path.lineTo(size * 0.80, size * 0.22);
        g.drawPath(path);
        g.setPen(QPen(kAccent, pen.widthF()));
        if (name == "chamfer") {
            g.drawLine(QPointF(size * 0.22, size * 0.42), QPointF(size * 0.42, size * 0.22));
        }
    } else if (name == "shell") {
        g.drawRect(QRectF(size * 0.20, size * 0.24, size * 0.60, size * 0.56));
        g.setPen(QPen(kAccent, pen.widthF()));
        g.drawRect(QRectF(size * 0.32, size * 0.36, size * 0.36, size * 0.32));
    } else if (name == "hole") {
        g.drawRect(QRectF(size * 0.18, size * 0.30, size * 0.64, size * 0.46));
        g.setPen(QPen(kAccent, pen.widthF()));
        g.drawEllipse(QRectF(size * 0.42, size * 0.38, size * 0.18, size * 0.18));
    } else if (name == "pattern-rect" || name == "pattern-circular" || name == "mirror") {
        const qreal d = size * 0.16;
        if (name == "pattern-rect") {
            for (int r = 0; r < 2; ++r) {
                for (int col = 0; col < 2; ++col) {
                    g.setPen(r == 0 && col == 0 ? pen : QPen(kAccent, pen.widthF()));
                    g.drawRect(QRectF(size * 0.24 + col * size * 0.30,
                                      size * 0.24 + r * size * 0.30, d, d));
                }
            }
        } else if (name == "pattern-circular") {
            g.drawRect(QRectF(size * 0.42, size * 0.16, d, d));
            g.setPen(QPen(kAccent, pen.widthF()));
            g.drawRect(QRectF(size * 0.68, size * 0.60, d, d));
            g.drawRect(QRectF(size * 0.16, size * 0.60, d, d));
        } else {
            g.drawRect(QRectF(size * 0.16, size * 0.34, size * 0.26, size * 0.32));
            g.setPen(QPen(kAccent, pen.widthF()));
            g.drawRect(QRectF(size * 0.58, size * 0.34, size * 0.26, size * 0.32));
            g.drawLine(QPointF(size * 0.50, size * 0.18), QPointF(size * 0.50, size * 0.82));
        }
    } else if (name == "measure") {
        g.drawLine(QPointF(size * 0.20, size * 0.66), QPointF(size * 0.80, size * 0.66));
        g.drawLine(QPointF(size * 0.20, size * 0.56), QPointF(size * 0.20, size * 0.76));
        g.drawLine(QPointF(size * 0.80, size * 0.56), QPointF(size * 0.80, size * 0.76));
        g.setPen(QPen(kAccent, pen.widthF()));
        g.drawLine(QPointF(size * 0.30, size * 0.36), QPointF(size * 0.70, size * 0.36));
    } else if (name == "section") {
        g.drawRect(QRectF(size * 0.20, size * 0.26, size * 0.60, size * 0.48));
        g.setPen(QPen(kAccent, pen.widthF()));
        g.drawLine(QPointF(size * 0.50, size * 0.16), QPointF(size * 0.50, size * 0.84));
        g.drawLine(QPointF(size * 0.54, size * 0.34), QPointF(size * 0.74, size * 0.34));
        g.drawLine(QPointF(size * 0.54, size * 0.50), QPointF(size * 0.74, size * 0.50));
    } else if (name == "mass" || name == "draft") {
        drawBox(g, size);
        g.setPen(QPen(kAccent, pen.widthF()));
        if (name == "mass") {
            g.drawEllipse(QRectF(size * 0.44, size * 0.44, size * 0.12, size * 0.12));
        } else {
            g.drawLine(QPointF(size * 0.30, size * 0.78), QPointF(size * 0.60, size * 0.30));
        }
    } else if (name == "dimension") {
        g.drawLine(QPointF(size * 0.20, size * 0.62), QPointF(size * 0.80, size * 0.62));
        g.drawLine(QPointF(size * 0.20, size * 0.52), QPointF(size * 0.20, size * 0.72));
        g.drawLine(QPointF(size * 0.80, size * 0.52), QPointF(size * 0.80, size * 0.72));
        g.setPen(QPen(kAccent, pen.widthF()));
        g.drawLine(QPointF(size * 0.40, size * 0.34), QPointF(size * 0.60, size * 0.34));
    } else if (name == "note") {
        g.drawRect(QRectF(size * 0.20, size * 0.22, size * 0.60, size * 0.44));
        g.setPen(QPen(kAccent, pen.widthF()));
        g.drawLine(QPointF(size * 0.30, size * 0.66), QPointF(size * 0.30, size * 0.82));
    } else if (name == "parameters") {
        for (int r = 0; r < 3; ++r) {
            const qreal y = size * (0.30 + r * 0.18);
            g.drawLine(QPointF(size * 0.20, y), QPointF(size * 0.80, y));
        }
        g.setPen(QPen(kAccent, pen.widthF()));
        g.drawEllipse(QRectF(size * 0.54, size * 0.24, size * 0.12, size * 0.12));
        g.drawEllipse(QRectF(size * 0.28, size * 0.42, size * 0.12, size * 0.12));
    } else if (name == "cache" || name == "purge") {
        // A stack of platters: the DDC as a store, not a folder.
        for (int r = 0; r < 3; ++r) {
            const qreal y = size * (0.28 + r * 0.18);
            g.drawEllipse(QRectF(size * 0.22, y, size * 0.56, size * 0.16));
        }
        if (name == "purge") {
            g.setPen(QPen(kAccent, pen.widthF()));
            g.drawLine(QPointF(size * 0.26, size * 0.78), QPointF(size * 0.74, size * 0.26));
        }
    } else if (name == "shaded" || name == "shaded-edges" || name == "wireframe") {
        drawBox(g, size);
        if (name != "wireframe") {
            g.setPen(Qt::NoPen);
            g.setBrush(name == "shaded" ? kLine : kAccent);
            g.drawRect(QRectF(size * 0.30, size * 0.40, size * 0.28, size * 0.28));
            g.setBrush(Qt::NoBrush);
        }
    } else if (name == "origin" || name == "sketches") {
        g.drawRect(QRectF(size * 0.18, size * 0.32, size * 0.44, size * 0.44));
        g.setPen(QPen(kAccent, pen.widthF()));
        g.drawRect(QRectF(size * 0.38, size * 0.22, size * 0.44, size * 0.44));
    } else if (name == "view-list" || name == "view-grid") {
        if (name == "view-list") {
            for (int r = 0; r < 3; ++r) {
                const qreal y = size * (0.28 + r * 0.20);
                g.drawLine(QPointF(size * 0.22, y), QPointF(size * 0.28, y));
                g.drawLine(QPointF(size * 0.38, y), QPointF(size * 0.78, y));
            }
        } else {
            for (int r = 0; r < 2; ++r) {
                for (int c = 0; c < 2; ++c) {
                    g.drawRect(QRectF(size * (0.22 + c * 0.30), size * (0.22 + r * 0.30),
                                      size * 0.22, size * 0.22));
                }
            }
        }
    // ── 3D Model / Create ───────────────────────────────────────────────────────────────
    //
    // Each glyph shows the OPERATION, not a generic solid: a profile plus what happens to it.
    // The accent colour marks the profile or path being consumed, the dark stroke the result.
    // With 3-per-column small buttons a user scans these at 16px, so the silhouettes have to
    // differ at a glance — near-identical glyphs are worse than no icons, because they read as
    // an interface that does not know what its own commands do.
    } else if (name == "sweep") {
        // A profile swept along a path: the path is the accent curve.
        g.setPen(QPen(kAccent, pen.widthF()));
        g.drawEllipse(QRectF(size * 0.14, size * 0.52, size * 0.20, size * 0.20));
        g.setPen(pen);
        QPainterPath path;
        path.moveTo(size * 0.24, size * 0.62);
        path.cubicTo(size * 0.50, size * 0.30, size * 0.60, size * 0.86, size * 0.84, size * 0.40);
        g.drawPath(path);
    } else if (name == "loft") {
        // Two profiles, blended. The two sections are the point.
        g.setPen(QPen(kAccent, pen.widthF()));
        g.drawEllipse(QRectF(size * 0.16, size * 0.16, size * 0.30, size * 0.14));
        g.drawEllipse(QRectF(size * 0.50, size * 0.68, size * 0.34, size * 0.16));
        g.setPen(pen);
        g.drawLine(QPointF(size * 0.18, size * 0.26), QPointF(size * 0.52, size * 0.76));
        g.drawLine(QPointF(size * 0.44, size * 0.24), QPointF(size * 0.82, size * 0.74));
    } else if (name == "coil") {
        // A helix. Three turns is enough to read; more becomes a scribble at 16px.
        g.drawLine(QPointF(size * 0.30, size * 0.16), QPointF(size * 0.30, size * 0.84));
        g.setPen(QPen(kAccent, pen.widthF()));
        for (int i = 0; i < 3; ++i) {
            g.drawArc(QRectF(size * 0.30, size * (0.20 + i * 0.21), size * 0.42, size * 0.20),
                      100 * 16, 220 * 16);
        }
    } else if (name == "emboss") {
        // Raised text/profile standing off a face.
        g.drawRect(QRectF(size * 0.16, size * 0.44, size * 0.68, size * 0.38));
        g.setPen(QPen(kAccent, pen.widthF()));
        g.drawRect(QRectF(size * 0.32, size * 0.22, size * 0.36, size * 0.22));
        g.drawLine(QPointF(size * 0.32, size * 0.44), QPointF(size * 0.32, size * 0.22));
    } else if (name == "derive") {
        // One part becoming another: box, arrow, box.
        g.drawRect(QRectF(size * 0.12, size * 0.34, size * 0.26, size * 0.32));
        g.drawRect(QRectF(size * 0.62, size * 0.34, size * 0.26, size * 0.32));
        g.setPen(QPen(kAccent, pen.widthF()));
        g.drawLine(QPointF(size * 0.42, size * 0.50), QPointF(size * 0.58, size * 0.50));
        g.drawLine(QPointF(size * 0.52, size * 0.44), QPointF(size * 0.58, size * 0.50));
        g.drawLine(QPointF(size * 0.52, size * 0.56), QPointF(size * 0.58, size * 0.50));
    } else if (name == "rib") {
        // A thin web between two walls.
        g.drawLine(QPointF(size * 0.18, size * 0.20), QPointF(size * 0.18, size * 0.80));
        g.drawLine(QPointF(size * 0.82, size * 0.20), QPointF(size * 0.82, size * 0.80));
        g.setPen(QPen(kAccent, pen.widthF()));
        g.drawRect(QRectF(size * 0.18, size * 0.44, size * 0.64, size * 0.16));
    } else if (name == "decal") {
        // An image applied to a face — the sheet is the accent.
        g.drawRect(QRectF(size * 0.14, size * 0.20, size * 0.50, size * 0.50));
        g.setPen(QPen(kAccent, pen.widthF()));
        g.drawRect(QRectF(size * 0.36, size * 0.40, size * 0.50, size * 0.42));
        g.drawLine(QPointF(size * 0.36, size * 0.70), QPointF(size * 0.52, size * 0.54));
        g.drawLine(QPointF(size * 0.52, size * 0.54), QPointF(size * 0.86, size * 0.78));
    } else if (name == "import") {
        // Into the part, from outside. Distinct from "open", which is a document.
        g.drawRect(QRectF(size * 0.14, size * 0.16, size * 0.44, size * 0.44));
        g.setPen(QPen(kAccent, pen.widthF()));
        g.drawLine(QPointF(size * 0.80, size * 0.76), QPointF(size * 0.42, size * 0.76));
        g.drawLine(QPointF(size * 0.52, size * 0.68), QPointF(size * 0.42, size * 0.76));
        g.drawLine(QPointF(size * 0.52, size * 0.84), QPointF(size * 0.42, size * 0.76));
    } else if (name == "unwrap") {
        // A curved face flattened to a plane.
        g.drawArc(QRectF(size * 0.10, size * 0.20, size * 0.40, size * 0.56), 90 * 16, 180 * 16);
        g.setPen(QPen(kAccent, pen.widthF()));
        g.drawRect(QRectF(size * 0.52, size * 0.32, size * 0.34, size * 0.36));

    // ── 3D Model / Modify ───────────────────────────────────────────────────────────────
    } else if (name == "thread") {
        // A threaded bore: the crest lines are what distinguishes it from Hole.
        g.drawRect(QRectF(size * 0.28, size * 0.14, size * 0.44, size * 0.72));
        g.setPen(QPen(kAccent, pen.widthF()));
        for (int i = 0; i < 4; ++i) {
            const qreal y = size * (0.24 + i * 0.16);
            g.drawLine(QPointF(size * 0.28, y), QPointF(size * 0.72, y + size * 0.07));
        }
    } else if (name == "split") {
        // One body cut in two by a plane, with the halves parted.
        g.drawRect(QRectF(size * 0.16, size * 0.16, size * 0.28, size * 0.68));
        g.drawRect(QRectF(size * 0.56, size * 0.16, size * 0.28, size * 0.68));
        g.setPen(QPen(kAccent, pen.widthF()));
        g.drawLine(QPointF(size * 0.50, size * 0.10), QPointF(size * 0.50, size * 0.90));
    } else if (name == "mark") {
        // A scribed line on a face — no material removed, which is the whole distinction.
        g.drawRect(QRectF(size * 0.16, size * 0.16, size * 0.68, size * 0.68));
        g.setPen(QPen(kAccent, pen.widthF()));
        g.drawLine(QPointF(size * 0.26, size * 0.70), QPointF(size * 0.74, size * 0.30));
    } else if (name == "combine") {
        // Two overlapping bodies: the boolean. Inventor's Combine is Join / Cut / Intersect.
        g.drawRect(QRectF(size * 0.14, size * 0.30, size * 0.42, size * 0.42));
        g.setPen(QPen(kAccent, pen.widthF()));
        g.drawRect(QRectF(size * 0.42, size * 0.20, size * 0.42, size * 0.42));
    } else if (name == "direct") {
        // Direct editing: a face pushed by a handle.
        g.drawRect(QRectF(size * 0.16, size * 0.24, size * 0.40, size * 0.52));
        g.setPen(QPen(kAccent, pen.widthF()));
        g.drawLine(QPointF(size * 0.60, size * 0.50), QPointF(size * 0.86, size * 0.50));
        g.drawLine(QPointF(size * 0.78, size * 0.42), QPointF(size * 0.86, size * 0.50));
        g.drawLine(QPointF(size * 0.78, size * 0.58), QPointF(size * 0.86, size * 0.50));
    } else if (name == "finish") {
        // A surface finish callout: a tick against a face.
        g.drawLine(QPointF(size * 0.16, size * 0.76), QPointF(size * 0.84, size * 0.76));
        g.setPen(QPen(kAccent, pen.widthF()));
        g.drawLine(QPointF(size * 0.40, size * 0.76), QPointF(size * 0.54, size * 0.28));
        g.drawLine(QPointF(size * 0.54, size * 0.28), QPointF(size * 0.68, size * 0.76));
    } else if (name == "thicken") {
        // A face offset to a solid: original plus its offset copy.
        g.drawRect(QRectF(size * 0.18, size * 0.28, size * 0.52, size * 0.34));
        g.setPen(QPen(kAccent, pen.widthF()));
        g.drawRect(QRectF(size * 0.30, size * 0.40, size * 0.52, size * 0.34));
    } else if (name == "delete-face") {
        // A face removed, leaving the opening. Dashed = what is gone.
        g.drawRect(QRectF(size * 0.16, size * 0.16, size * 0.68, size * 0.68));
        QPen dashed(kAccent, pen.widthF());
        dashed.setStyle(Qt::DashLine);
        g.setPen(dashed);
        g.drawRect(QRectF(size * 0.32, size * 0.32, size * 0.36, size * 0.36));
    } else if (name == "horizontal" || name == "vertical") {
        // The constrained line in the accent colour against a faint axis, so the two read as a
        // pair and the direction is unmistakable at 16px.
        const bool horiz = name == "horizontal";
        g.setPen(QPen(QColor(0xc9, 0xc7, 0xc3), pen.widthF()));
        if (horiz) g.drawLine(QPointF(size * 0.12, size * 0.72), QPointF(size * 0.88, size * 0.72));
        else       g.drawLine(QPointF(size * 0.72, size * 0.12), QPointF(size * 0.72, size * 0.88));
        g.setPen(QPen(kAccent, pen.widthF() * 1.5));
        if (horiz) g.drawLine(QPointF(size * 0.16, size * 0.36), QPointF(size * 0.84, size * 0.36));
        else       g.drawLine(QPointF(size * 0.36, size * 0.16), QPointF(size * 0.36, size * 0.84));
    } else if (name == "parallel" || name == "perpendicular") {
        g.setPen(QPen(kLine, pen.widthF() * 1.3));
        if (name == "parallel") {
            g.drawLine(QPointF(size * 0.24, size * 0.82), QPointF(size * 0.52, size * 0.18));
            g.setPen(QPen(kAccent, pen.widthF() * 1.3));
            g.drawLine(QPointF(size * 0.52, size * 0.82), QPointF(size * 0.80, size * 0.18));
        } else {
            g.drawLine(QPointF(size * 0.20, size * 0.78), QPointF(size * 0.80, size * 0.78));
            g.setPen(QPen(kAccent, pen.widthF() * 1.3));
            g.drawLine(QPointF(size * 0.38, size * 0.78), QPointF(size * 0.38, size * 0.20));
        }
    } else if (name == "equal") {
        // Two equals bars, the mathematical sign, over two segments of matching length.
        g.setPen(QPen(kLine, pen.widthF() * 1.3));
        g.drawLine(QPointF(size * 0.14, size * 0.74), QPointF(size * 0.44, size * 0.74));
        g.drawLine(QPointF(size * 0.56, size * 0.74), QPointF(size * 0.86, size * 0.74));
        g.setPen(QPen(kAccent, pen.widthF()));
        g.drawLine(QPointF(size * 0.30, size * 0.30), QPointF(size * 0.70, size * 0.30));
        g.drawLine(QPointF(size * 0.30, size * 0.46), QPointF(size * 0.70, size * 0.46));
    } else if (name == "radius") {
        // An arc with a radius line and an arrowhead: the dimension itself, not a generic circle.
        g.drawArc(QRectF(size * 0.14, size * 0.14, size * 0.72, size * 0.72), 0, 270 * 16);
        g.setPen(QPen(kAccent, pen.widthF()));
        const QPointF c(size * 0.50, size * 0.50);
        const QPointF rim(size * 0.82, size * 0.50);
        g.drawLine(c, rim);
        g.drawLine(rim, rim + QPointF(-size * 0.09, -size * 0.05));
        g.drawLine(rim, rim + QPointF(-size * 0.09, size * 0.05));
    } else if (name == "select") {
        // An arrow cursor over a curve: the pointer is what every application uses for "pick".
        QPainterPath arrow;
        arrow.moveTo(size * 0.30, size * 0.18);
        arrow.lineTo(size * 0.30, size * 0.72);
        arrow.lineTo(size * 0.44, size * 0.58);
        arrow.lineTo(size * 0.54, size * 0.80);
        arrow.lineTo(size * 0.64, size * 0.74);
        arrow.lineTo(size * 0.54, size * 0.53);
        arrow.lineTo(size * 0.70, size * 0.50);
        arrow.closeSubpath();
        g.drawPath(arrow);
    } else if (name == "rollback" || name == "rollforward") {
        // The tree as three bars with the marker between them. Which bars are dimmed says which
        // features are suspended, so the two directions read differently at a glance.
        const bool back = name == "rollback";
        for (int r = 0; r < 3; ++r) {
            const qreal y = size * (0.24 + r * 0.22);
            const bool suspended = back ? r == 2 : r == 0;
            g.setPen(QPen(suspended ? QColor(0xa8, 0xab, 0xaf) : kLine, pen.widthF()));
            g.drawLine(QPointF(size * 0.20, y), QPointF(size * 0.80, y));
        }
        g.setPen(QPen(kAccent, pen.widthF() * 1.4));
        const qreal marker = back ? size * 0.57 : size * 0.35;
        g.drawLine(QPointF(size * 0.14, marker), QPointF(size * 0.86, marker));
    } else {
        g.drawRect(QRectF(size * 0.28, size * 0.28, size * 0.44, size * 0.44));
    }
    g.end();
    return QIcon(pm);
}

}  // namespace cadqt
