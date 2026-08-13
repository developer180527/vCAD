#include "Viewport.h"

#include <QImage>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>

#include <algorithm>
#include <cmath>
#include <QWheelEvent>

namespace cadqt {
namespace {

/// Column-major 4x4 times a 4-vector. Full w, no shortcuts: the placeholder projects with the
/// SAME matrices the GPU will, so if a box lands in the wrong place here it would land in the
/// wrong place there. Getting this approximately right would defeat the purpose.
void transform4(const float m[16], const float in[4], float out[4]) {
    for (int r = 0; r < 4; ++r) {
        out[r] = m[r] * in[0] + m[4 + r] * in[1] + m[8 + r] * in[2] + m[12 + r] * in[3];
    }
}

}  // namespace

Viewport::Viewport(cad::app::Controller& controller, QWidget* parent)
    : QWidget(parent), controller_(controller) {
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setAutoFillBackground(false);
    setMinimumSize(320, 240);
}

QPointF Viewport::project(const float world[3], bool& visible) const {
    const auto& camera = controller_.frame().camera;
    const float p[4]{world[0], world[1], world[2], 1.0f};
    float eye[4];
    float clip[4];
    transform4(camera.view.m, p, eye);
    transform4(camera.projection.m, eye, clip);

    // w comes from the projection, not from a guess about the camera mode. Orthographic
    // projections leave it at 1; perspective ones do not, and hard-coding either is how a
    // viewport ends up correct in one mode and subtly wrong in the other.
    visible = clip[3] > 1e-6f;
    if (!visible) return {};

    const float ndcX = clip[0] / clip[3];
    const float ndcY = clip[1] / clip[3];
    return QPointF((ndcX * 0.5f + 0.5f) * width(), (1.0f - (ndcY * 0.5f + 0.5f)) * height());
}

bool Viewport::attachRenderer() {
    if (attached_) return true;
    syncViewportSize();
    const auto dpr = devicePixelRatioF();
    auto r = controller_.attachRenderer(
        static_cast<std::uint32_t>(std::max(1.0, width() * dpr)),
        static_cast<std::uint32_t>(std::max(1.0, height() * dpr)));
    if (!r) {
        rendererError_ = QString::fromStdString(r.error().message);
        return false;
    }
    attached_ = true;
    rendererError_.clear();
    // Clear to the theme's own surface colour, read from the palette rather than restated as a
    // literal, so the viewport cannot drift away from the rest of the window.
    const QColor paper = palette().color(QPalette::Base);
    controller_.setViewportBackground(paper.red(), paper.green(), paper.blue());
    update();
    return true;
}

void Viewport::markDirty() {
    dirty_ = true;
    update();
}

void Viewport::syncViewportSize() {
    // DEVICE pixels, not logical ones. Getting this wrong renders a half-resolution frame and
    // stretches it over a Retina widget, which reads as "the renderer is blurry" rather than as
    // a units mistake -- the same class of bug as the blurry ribbon icons.
    const auto dpr = devicePixelRatioF();
    controller_.setViewportSize(static_cast<std::uint32_t>(std::max(1.0, width() * dpr)),
                                static_cast<std::uint32_t>(std::max(1.0, height() * dpr)));
}

void Viewport::paintEvent(QPaintEvent*) {
    QPainter g(this);
    g.setRenderHint(QPainter::Antialiasing, true);
    g.fillRect(rect(), palette().color(QPalette::Base));

    if (attached_) {
        if (dirty_ || frame_.isNull()) {
            auto rendered = controller_.renderFrame();
            if (rendered) {
                // The renderer's OWN dimensions, never the widget's. The two disagree for a
                // moment after a resize, and a stride computed from the wrong one skews rows.
                const int w = static_cast<int>(rendered.value().width);
                const int h = static_cast<int>(rendered.value().height);
                pixels_ = std::move(rendered.value().pixels);
                const auto need = static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4;
                if (w > 0 && h > 0 && pixels_.size() >= need) {
                    // RGBA8888 rather than ARGB32: the backend reads back RGBA8, and
                    // Format_ARGB32 is BGRA in memory on a little-endian machine. That
                    // difference is a red/blue swap, which looks like a plausible shading
                    // choice rather than a bug.
                    frame_ = QImage(pixels_.data(), w, h, w * 4, QImage::Format_RGBA8888);
                    // The image is in DEVICE pixels. Without this Qt treats it as logical and
                    // rescales it on the CPU every paint -- on a Retina display that is a
                    // 4x-too-large image resampled per frame, and it was most of the cost that
                    // made this feel like 15fps.
                    frame_.setDevicePixelRatio(devicePixelRatioF());
                    dirty_ = false;
                }
            }
        }
        if (!frame_.isNull()) {
            g.drawImage(QPoint(0, 0), frame_);
            return;
        }
        // A failed capture falls through to the wireframe rather than painting nothing, so a
        // renderer that stops working is visible as a downgrade instead of a black rectangle.
    }

    paintFallback(g);
}

void Viewport::paintFallback(QPainter& g) {

    const auto stats = controller_.stats();
    if (stats.objects == 0) {
        g.setPen(QColor(0x5d, 0x63, 0x6b));
        g.drawText(rect(), Qt::AlignCenter,
                   tr("Create a feature from the ribbon to begin"));
        return;
    }

    // The scene's real bounding box, wireframed, plus a ground grid.
    //
    // Not a renderer — a wiring check. If this box sits where it should and responds correctly to
    // orbit, pan and zoom, then the document, the recompute, the tessellation bounds and the
    // camera matrices are all correct, and the GPU backend has one job instead of five. Given how
    // M3.3 went, having this known-good reference is worth more than it looks.
    const auto b = controller_.bounds();
    if (!b.valid()) return;

    // Grid on the Z=min plane, so the part sits on it. CAD users read scale from a grid far more
    // reliably than from a lone wireframe box floating in space.
    const float span = std::max({b.max[0] - b.min[0], b.max[1] - b.min[1], 1.0f});
    const float step = std::pow(10.0f, std::floor(std::log10(span))) / 2.0f;
    if (step > 0.0f) {
        g.setPen(QPen(QColor(0x2d, 0x31, 0x36), 1.0));
        const float lo = std::floor(b.min[0] / step - 4) * step;
        const float hi = std::ceil(b.max[0] / step + 4) * step;
        const float lo2 = std::floor(b.min[1] / step - 4) * step;
        const float hi2 = std::ceil(b.max[1] / step + 4) * step;
        for (float x = lo; x <= hi; x += step) {
            bool a = false, c = false;
            const float p0[3]{x, lo2, b.min[2]};
            const float p1[3]{x, hi2, b.min[2]};
            const QPointF q0 = project(p0, a);
            const QPointF q1 = project(p1, c);
            if (a && c) g.drawLine(q0, q1);
        }
        for (float y = lo2; y <= hi2; y += step) {
            bool a = false, c = false;
            const float p0[3]{lo, y, b.min[2]};
            const float p1[3]{hi, y, b.min[2]};
            const QPointF q0 = project(p0, a);
            const QPointF q1 = project(p1, c);
            if (a && c) g.drawLine(q0, q1);
        }
    }

    static constexpr int edges[12][2]{{0, 1}, {1, 3}, {3, 2}, {2, 0}, {4, 5}, {5, 7},
                                      {7, 6}, {6, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    QPointF corners[8];
    bool ok[8];
    for (int i = 0; i < 8; ++i) {
        const float p[3]{(i & 1) ? b.max[0] : b.min[0], (i & 2) ? b.max[1] : b.min[1],
                         (i & 4) ? b.max[2] : b.min[2]};
        corners[i] = project(p, ok[i]);
    }
    g.setPen(QPen(QColor(0x4d, 0x9e, 0xe8), 1.4));
    for (const auto& e : edges) {
        if (ok[e[0]] && ok[e[1]]) g.drawLine(corners[e[0]], corners[e[1]]);
    }

    g.setPen(QColor(0x76, 0x7d, 0x87));
    g.drawText(12, height() - 14,
               tr("wireframe fallback — no GPU renderer — %1 object(s), %2 mesh(es), %3 triangles")
                   .arg(stats.objects)
                   .arg(stats.uniqueMeshes)
                   .arg(stats.triangles));

    // A ViewCube stand-in, so the corner is reserved and the layout is honest about what goes
    // there rather than looking finished and then shifting.
    const QRectF cube(width() - 78.0, 12.0, 56.0, 56.0);
    g.setPen(QPen(QColor(0x52, 0x57, 0x5e), 1.0));
    g.drawRect(cube);
    g.setPen(QColor(0x76, 0x7d, 0x87));
    g.drawText(cube, Qt::AlignCenter, controller_.camera().orthographic() ? tr("ORTHO")
                                                                        : tr("PERSP"));
}

void Viewport::resizeEvent(QResizeEvent*) {
    syncViewportSize();
    markDirty();   // the cached frame is the wrong size now
}

void Viewport::mousePressEvent(QMouseEvent* event) {
    lastMouse_ = event->pos();
    const int button = event->button() == Qt::LeftButton ? 0
                       : event->button() == Qt::MiddleButton ? 1 : 2;
    // Gesture mapping comes from the controller so both shells behave identically instead of
    // each reimplementing the preset table.
    drag_ = controller_.camera().dragFor(button,
                                        event->modifiers().testFlag(Qt::ShiftModifier),
                                        event->modifiers().testFlag(Qt::ControlModifier));
}

void Viewport::mouseMoveEvent(QMouseEvent* event) {
    if (drag_ == cad::render::Drag::None) return;
    const QPoint delta = event->pos() - lastMouse_;
    lastMouse_ = event->pos();

    switch (drag_) {
        case cad::render::Drag::Orbit:
            controller_.camera().orbit(float(delta.x()), float(delta.y()));
            break;
        case cad::render::Drag::Pan:
            controller_.camera().pan(float(delta.x()), float(delta.y()),
                                    cad::render::Viewport{std::uint32_t(width()),
                                                          std::uint32_t(height()), 1.0f});
            break;
        case cad::render::Drag::Zoom:
            controller_.camera().zoom(float(-delta.y()) * 0.1f);
            break;
        case cad::render::Drag::None:
            break;
    }
    // cameraChanged, not setViewportSize: the widget did not resize, and the full size path ran
    // two culls to deliver one camera update. It is still REQUIRED to call something here --
    // mutating the camera does not move the scene's copy of the matrices.
    controller_.cameraChanged();
    markDirty();
}

void Viewport::mouseReleaseEvent(QMouseEvent*) { drag_ = cad::render::Drag::None; }

void Viewport::wheelEvent(QWheelEvent* event) {
    controller_.camera().zoom(float(event->angleDelta().y()) / 120.0f);
    controller_.cameraChanged();
    markDirty();
}

}  // namespace cadqt
