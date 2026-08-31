#include "Viewport.h"

#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QGuiApplication>
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

    // Created up front and hidden, rather than made on demand: a widget constructed during a mouse
    // move is a widget constructed sixty times a second.
    // A TOP-LEVEL window, not a child of this widget — `this` is only its owner.
    //
    // A child of a WA_PaintOnScreen widget has no defined compositing: this viewport hands its
    // pixels to a CAMetalLayer and Qt is told not to paint it, so a child ends up drawn into a
    // surface nothing ever clears. The result was catastrophic and immediate — the viewport went
    // black and the readout smeared a trail of itself across it on every mouse move.
    //
    // Qt::ToolTip makes it a borderless top-level that the window server composites over the Metal
    // layer, which is how Qt's own tooltips manage to appear over a 3D view. It also never takes
    // focus, so keystrokes still reach the viewport — which the dimension entry depends on.
    dimensionField_ = new QLabel(this, Qt::ToolTip);
    dimensionField_->setObjectName(QStringLiteral("sketchDimension"));
    dimensionField_->setFocusPolicy(Qt::NoFocus);
    dimensionField_->hide();
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

QPaintEngine* Viewport::paintEngine() const { return native_ ? nullptr : QWidget::paintEngine(); }

namespace {
/// Process-wide, and set before any viewport exists. Not per-instance: the choice comes from the
/// command line, and a window with one viewport presenting and another blitting would be a
/// harder thing to reason about than anything it could buy.
bool g_forceOffscreen = false;
}  // namespace

void Viewport::setForceOffscreen(bool force) noexcept { g_forceOffscreen = force; }

bool Viewport::attachRenderer() {
    if (attached_) return true;
    syncViewportSize();
    const auto dpr = devicePixelRatioF();
    const auto w = static_cast<std::uint32_t>(std::max(1.0, width() * dpr));
    const auto h = static_cast<std::uint32_t>(std::max(1.0, height() * dpr));

    void* view = nullptr;
    // Only a platform that has REAL native windows can hand one over.
    //
    // `winId()` returns a handle whatever the platform is, and under the offscreen and minimal QPA
    // plugins it is not an NSView — passing it to the surface code sends a message to a pointer
    // that is not an object, which crashes in objc_msgSend with no diagnostic at all. That is what
    // stopped the shell from running headless, and therefore what stopped it from ever being
    // tested.
    const QString platform = QGuiApplication::platformName();
    const bool nativeWindows = platform != QStringLiteral("offscreen")
                               && platform != QStringLiteral("minimal");
    if (!g_forceOffscreen && nativeWindows) {
        // Force a real native view, then hand it over. winId() is what creates it; asking for it
        // is the entire mechanism, which is why it looks like a discarded value.
        setAttribute(Qt::WA_NativeWindow, true);
        view = reinterpret_cast<void*>(winId());
    }

    auto r = controller_.attachRenderer(w, h, view, dpr);
    if (!r && view != nullptr) {
        // Fall back to the offscreen path rather than failing outright. It is slower, but a slow
        // viewport is a working application and no viewport is not.
        //
        // This is only a fallback because attachRenderer releases the surface it built before
        // returning the error. It did not, once, and the retry inherited the native attempt's
        // layer and rebuilt the identical on-screen configuration — a fallback that fell back to
        // exactly what had just failed.
        rendererError_ = QString::fromStdString(r.error().message);
        r = controller_.attachRenderer(w, h, nullptr, dpr);
    }
    if (!r) {
        if (rendererError_.isEmpty()) rendererError_ = QString::fromStdString(r.error().message);
        return false;
    }
    native_ = controller_.presentsDirectly();
    if (native_) {
        // The widget owns every pixel in its rectangle from here. Qt must not draw a background
        // under the layer, must not double-buffer it, and must not composite anything over it.
        setAttribute(Qt::WA_PaintOnScreen, true);
        setAttribute(Qt::WA_NoSystemBackground, true);
        setAttribute(Qt::WA_OpaquePaintEvent, true);
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
    // Here rather than beside every edit, because the labels have to follow the CAMERA too: an
    // orbit changes where a dimension belongs on screen without changing the sketch at all. Every
    // one of those paths already ends in a repaint, so this is the one hook that catches them all.
    // Showing a separate top-level window does not repaint this one, so there is no recursion.
    if (controller_.environment() == cad::app::Environment::Sketch) {
        syncDimensionLabels();
    } else if (!dimensionLabels_.empty()) {
        for (QLabel* label : dimensionLabels_) label->hide();
    }

    if (native_) {
        // No QPainter, no image, no readback: submit and the GPU presents. This is the whole
        // point of the native path, and the reason paintEngine() returns null.
        controller_.presentFrame();
        return;
    }

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
    // A drawing click, handled before the gesture mapping: while a tool is active the left button
    // draws rather than selects, which is how every CAD sketcher behaves. Navigation still works,
    // because orbit and pan are on the middle button and on Alt.
    if (event->button() == Qt::LeftButton && controller_.leftPressDraws()
        && !event->modifiers().testFlag(Qt::AltModifier)) {
        const auto dpr = devicePixelRatioF();
        lastMouse_ = event->pos();
        if (controller_.sketchClickAt(static_cast<float>(event->position().x() * dpr),
                                      static_cast<float>(event->position().y() * dpr))) {
            markDirty();
            update();
        }
        syncDimensionField();
        drag_ = cad::render::Drag::None;
        return;
    }

    lastMouse_ = event->pos();
    pressAt_ = event->pos();
    dragged_ = false;
    const int button = event->button() == Qt::LeftButton ? 0
                       : event->button() == Qt::MiddleButton ? 1 : 2;
    // Gesture mapping comes from the controller so both shells behave identically instead of
    // each reimplementing the preset table.
    // Orbit mode makes a plain left drag rotate. Checked before the preset table because it is a
    // deliberate mode the user turned on, and it has to beat the left-is-selection rule that the
    // table enforces.
    if (controller_.orbitMode() && event->button() == Qt::LeftButton) {
        drag_ = cad::render::Drag::Orbit;
        return;
    }

    drag_ = controller_.camera().dragFor(button,
                                        event->modifiers().testFlag(Qt::ShiftModifier),
                                        event->modifiers().testFlag(Qt::ControlModifier),
                                        event->modifiers().testFlag(Qt::AltModifier));
}

bool Viewport::focusNextPrevChild(bool next) {
    if (controller_.environment() == cad::app::Environment::Sketch
        && controller_.sketchPending()) {
        return false;   // Tab belongs to the dimension field while a shape is half-drawn
    }
    return QWidget::focusNextPrevChild(next);
}

void Viewport::hideEvent(QHideEvent* event) {
    // A top-level readout does not disappear with its owner. Left alone it floats over whatever the
    // user switches to, which is worse than not having it.
    if (dimensionField_ != nullptr) dimensionField_->hide();
    for (QLabel* label : dimensionLabels_) label->hide();
    QWidget::hideEvent(event);
}

void Viewport::leaveEvent(QEvent* event) {
    // The pointer has left the viewport, so there is nothing for the readout to annotate.
    if (dimensionField_ != nullptr) dimensionField_->hide();
    for (QLabel* label : dimensionLabels_) label->hide();
    // And nothing is under it any more. A hover highlight left behind claims the pointer is
    // somewhere it is not.
    if (controller_.clearHover()) markDirty();
    QWidget::leaveEvent(event);
}

void Viewport::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && controller_.leftPressDraws()) {
        controller_.endSketchChain();
        syncDimensionField();
        markDirty();
        return;
    }

    // A double click selects the whole BODY, whatever the level says.
    //
    // The same rule as the iPad's double tap, and it is what makes Auto usable as a default: one
    // click takes the face or edge under the pointer, two take the part it belongs to. Without it,
    // selecting a body under Auto would mean going to the filter bar and back.
    if (event->button() == Qt::LeftButton
        && controller_.environment() != cad::app::Environment::Sketch) {
        const auto dpr = devicePixelRatioF();
        const auto x = static_cast<std::uint32_t>(std::max(0.0, event->position().x() * dpr));
        const auto y = static_cast<std::uint32_t>(std::max(0.0, event->position().y() * dpr));
        const auto radius = static_cast<std::uint32_t>(std::lround(4.0 * dpr));
        const auto result =
            controller_.tapAt(x, y, radius, event->modifiers().testFlag(Qt::ShiftModifier),
                              cad::app::Controller::SelectionLevel::Body);
        if (!result.message.empty()) emit pickMessage(QString::fromStdString(result.message));
        if (result.changed) markDirty();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

void Viewport::keyPressEvent(QKeyEvent* event) {
    // Escape abandons the plane pick, so a user who pressed Sketch by mistake is not stuck with a
    // viewport whose next click means something they did not ask for.
    if (event->key() == Qt::Key_Escape && controller_.awaitingSketchPlane()) {
        controller_.cancelSketchPlanePick();
        markDirty();
        return;
    }

    // Only while something is half-drawn. Outside that a digit is a shortcut, and swallowing it
    // here would make the keyboard feel dead — the Controller decides, so both shells agree.
    if (controller_.environment() == cad::app::Environment::Sketch) {
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
            if (controller_.commitSketchDimension()) {
                syncDimensionField();
                markDirty();
                return;
            }
        } else if (event->key() == Qt::Key_Tab) {
            if (controller_.lockSketchDimension()) {
                syncDimensionField();
            emit dimensionChanged();
                markDirty();
                return;
            }
        } else if (event->key() == Qt::Key_Backspace) {
            controller_.backspaceSketchDimension();
            syncDimensionField();
            emit dimensionChanged();
            return;
        } else if (event->key() == Qt::Key_Escape) {
            // Clears a half-typed number first, and only ENDS THE CHAIN when there is none —
            // otherwise one keystroke would throw away both the digits and the run being drawn.
            if (!controller_.sketchDimensionInput().empty()) {
                controller_.clearSketchDimension();
            } else {
                controller_.endSketchChain();
            }
            syncDimensionField();
            markDirty();
            return;
        } else if (!event->text().isEmpty()
                   && controller_.typeSketchDimension(event->text().at(0).toLatin1())) {
            syncDimensionField();
            return;
        }
    }
    QWidget::keyPressEvent(event);
}

void Viewport::syncDimensionLabels() {
    const auto labels = controller_.sketchDimensionLabels();

    // Grown, never shrunk: a sketch gains and loses dimensions constantly while being drawn, and
    // destroying windows on every edit would flicker. The surplus is hidden instead.
    while (dimensionLabels_.size() < labels.size()) {
        auto* label = new QLabel(this, Qt::ToolTip);
        label->setObjectName(QStringLiteral("sketchDimensionLabel"));
        label->setFocusPolicy(Qt::NoFocus);
        // Never takes the pointer: a dimension sitting on a curve must not stop the curve being
        // clicked, which is exactly where these end up.
        label->setAttribute(Qt::WA_TransparentForMouseEvents);
        dimensionLabels_.push_back(label);
    }

    const auto dpr = devicePixelRatioF();
    for (std::size_t i = 0; i < dimensionLabels_.size(); ++i) {
        QLabel* label = dimensionLabels_[i];
        if (i >= labels.size()) {
            label->hide();
            continue;
        }
        label->setText(QString::fromStdString(labels[i].text));
        label->adjustSize();

        // The controller projects in DEVICE pixels because that is what the viewport renders in;
        // widgets are placed in logical ones.
        const int x = static_cast<int>(labels[i].x / dpr) - label->width() / 2;
        const int y = static_cast<int>(labels[i].y / dpr) - label->height() - 6;

        // Clamped to the viewport, so a dimension on geometry scrolled half off screen still shows
        // where it can rather than wandering onto the ribbon.
        const int cx = std::clamp(x, 2, std::max(2, width() - label->width() - 2));
        const int cy = std::clamp(y, 2, std::max(2, height() - label->height() - 2));
        label->move(mapToGlobal(QPoint(cx, cy)));
        label->show();
    }
}

void Viewport::syncDimensionField() {
    if (dimensionField_ == nullptr) return;

    const auto measure = controller_.sketchPreviewMeasure();
    if (!measure.valid) {
        dimensionField_->hide();
        return;
    }

    // Formatted by the CONTROLLER, in the document's display units. A shell printing raw
    // millimetres would show a number the rest of the application does not use — and the units
    // preference is not the shell's to know about.
    const auto text = controller_.sketchPreviewText();
    if (!text.valid) {
        dimensionField_->hide();
        return;
    }
    const QString caret = controller_.sketchDimensionInput().empty() ? QString()
                                                                     : QStringLiteral("_");
    // A padlock while the length is fixed, so the user can see WHY the number stopped following
    // their mouse. Without it a locked field looks like a frozen one.
    const QString lock = controller_.sketchLockedLength() ? QStringLiteral(" \U0001F512")
                                                          : QString();
    const QString length = QString::fromStdString(text.length) + caret;
    dimensionField_->setText((text.angle.empty()
                                  ? length
                                  : QStringLiteral("%1   %2").arg(
                                        length, QString::fromStdString(text.angle)))
                             + lock);
    dimensionField_->adjustSize();

    // Offset from the cursor so the pointer never sits on top of the number, and clamped inside
    // the viewport so it cannot be pushed off-screen near an edge.
    // GLOBAL coordinates, because it is a window rather than a child. Still clamped to the
    // viewport's own rectangle so it cannot wander onto the ribbon or off the screen.
    const QPoint at = lastMouse_ + QPoint(16, 16);
    const int x = std::min(at.x(), width() - dimensionField_->width() - 4);
    const int y = std::min(at.y(), height() - dimensionField_->height() - 4);
    dimensionField_->move(mapToGlobal(QPoint(std::max(4, x), std::max(4, y))));
    dimensionField_->show();
}

void Viewport::mouseMoveEvent(QMouseEvent* event) {
    // The rubber band, before the drag check: a half-drawn shape follows the pointer with no button
    // held, which is the whole point of it.
    if (drag_ == cad::render::Drag::None
        && controller_.environment() == cad::app::Environment::Sketch
        && controller_.sketchPending()) {
        const auto dpr = devicePixelRatioF();
        lastMouse_ = event->pos();
        if (controller_.sketchHoverAt(static_cast<float>(event->position().x() * dpr),
                                      static_cast<float>(event->position().y() * dpr))) {
            markDirty();
            update();
        }
        syncDimensionField();
        // The status bar carries the same numbers, and a hover changes them without changing the
        // document — so nothing else would ask it to refresh.
        emit dimensionChanged();
        return;
    }
    // PRE-HIGHLIGHT under the pointer, which the desktop simply never did: `Controller::hoverAt`
    // existed, maintained a hovered slot and fed the highlight table, and no shell ever called it.
    // Hover is what tells a user WHICH of several overlapping things a click will take — without
    // it, fine-grained selection is a guess followed by a check.
    //
    // The same aperture as the click below, deliberately. A one-pixel hover would light up the face
    // while the click took the edge crossing it, and an answer arrived at by a different rule than
    // the action it predicts is not an answer.
    if (drag_ == cad::render::Drag::None) {
        const auto dpr = devicePixelRatioF();
        const auto radius = static_cast<std::uint32_t>(std::lround(4.0 * dpr));
        if (controller_.hoverAt(
                static_cast<std::uint32_t>(std::max(0.0, event->position().x() * dpr)),
                static_cast<std::uint32_t>(std::max(0.0, event->position().y() * dpr)), radius)) {
            markDirty();
        }
        return;
    }
    // Four logical pixels. Below that it is a click with a shaky hand, and treating it as an orbit
    // both fails to select and nudges the camera, which reads as the application ignoring clicks.
    if ((event->pos() - pressAt_).manhattanLength() > 4) dragged_ = true;
    const QPoint delta = event->pos() - lastMouse_;
    lastMouse_ = event->pos();

    switch (drag_) {
        case cad::render::Drag::Orbit:
            // Through the controller, which refuses while a sketch is open — the rule belongs to
            // the model so both shells obey it without each remembering to.
            controller_.orbitCamera(float(delta.x()), float(delta.y()));
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

void Viewport::mouseReleaseEvent(QMouseEvent* event) {
    const cad::render::Drag was = drag_;
    drag_ = cad::render::Drag::None;

    // A CLICK: left button, released without having dragged. This is the call the viewport never
    // made — `Controller::clickAt` has always done the whole job (pick, resolve at the current
    // selection level, select, highlight, notify), and nothing asked it to. Selecting from the Model
    // panel worked, which is why the picker looked broken rather than unused.
    if (was != cad::render::Drag::Orbit && was != cad::render::Drag::Pan
        && was != cad::render::Drag::Zoom) {
        // fall through: a button with no gesture mapped is still a click
    } else if (dragged_) {
        return;   // it was a gesture, not a click
    }
    if (event->button() != Qt::LeftButton) return;

    // DEVICE pixels. The id buffer is indexed in them, and forwarding logical coordinates picks at
    // half the intended position on a Retina display — a bug that looks like an inaccurate picker
    // rather than a units mistake. Controller::pickAt documents the same requirement.
    const auto dpr = devicePixelRatioF();
    const auto x = static_cast<std::uint32_t>(std::max(0.0, event->position().x() * dpr));
    const auto y = static_cast<std::uint32_t>(std::max(0.0, event->position().y() * dpr));

    // CHOOSING A SKETCH PLANE takes the click before selection sees it.
    //
    // Start Sketch with nothing selected no longer guesses a plane (MODELLING_UX.md §2: no CAD
    // silently chooses one for you) — it asks, and this is the answer.
    if (controller_.awaitingSketchPlane()) {
        const auto dpr2 = devicePixelRatioF();
        controller_.sketchOnPickedPlane(
            static_cast<std::uint32_t>(std::max(0.0, event->position().x() * dpr2)),
            static_cast<std::uint32_t>(std::max(0.0, event->position().y() * dpr2)));
        markDirty();
        return;
    }

    // An aperture even for a mouse, and a small one.
    //
    // A single-pixel hit test makes edge and vertex selection a test of motor control: an edge is
    // one pixel wide on screen, so clicking one means landing on it exactly. Every CAD application
    // has a tolerance of a few pixels for this reason — see docs/design/SELECTION.md. The tablet
    // passes a fingertip instead, and that radius is the ONLY difference between the two shells'
    // selection behaviour.
    //
    // Scaled by the device pixel ratio because it is specified in logical pixels: 4 physical pixels
    // on a Retina display is half the tolerance the same code gives a non-Retina one.
    const auto radius = static_cast<std::uint32_t>(std::lround(4.0 * dpr));
    const auto result =
        controller_.tapAt(x, y, radius, event->modifiers().testFlag(Qt::ShiftModifier));
    if (!result.message.empty()) emit pickMessage(QString::fromStdString(result.message));
    if (result.changed) markDirty();
}

void Viewport::wheelEvent(QWheelEvent* event) {
    controller_.camera().zoom(float(event->angleDelta().y()) / 120.0f);
    controller_.cameraChanged();
    markDirty();
}

}  // namespace cadqt
