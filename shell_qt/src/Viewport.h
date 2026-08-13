#pragma once

#include "cad/app/Controller.h"

#include <QImage>
#include <QString>
#include <QWidget>

#include <cstdint>
#include <vector>

class QPainter;

namespace cadqt {

/// The 3D viewport.
///
/// Renders through bgfx when a GPU is available, and falls back to a QPainter wireframe of the
/// scene bounds and a ground grid when one is not. Both paths read the SAME `SceneFrame` and the
/// same camera matrices, which is why the fallback is kept rather than deleted: it is a
/// known-good reference for "is the camera right" that is independent of the GPU, and it is the
/// only thing that draws at all on a machine with no graphics stack.
///
/// The GPU path is offscreen-and-blit: the backend renders into a framebuffer, we read it back
/// and paint it into the widget. That costs a full readback per frame and is not the endgame —
/// see Controller::attachRenderer — but it composites correctly with every Qt overlay the shell
/// already has, because as far as Qt is concerned this is an ordinary widget with an image on it.
class Viewport : public QWidget {
    Q_OBJECT
public:
    explicit Viewport(cad::app::Controller& controller, QWidget* parent = nullptr);

    /// Brings up the GPU renderer. Returns false and leaves the fallback in place if there is no
    /// usable device — not fatal: the shell stays usable and rendererError() says why.
    bool attachRenderer();

    [[nodiscard]] bool rendererAttached() const noexcept { return attached_; }

    /// Marks the rendered frame stale. Repainting does NOT re-render by itself: an offscreen
    /// frame costs a GPU readback, and Qt repaints a widget for many reasons that have nothing
    /// to do with the scene — a ribbon hover, a tooltip, an overlapping panel. Re-rendering on
    /// each of those is most of what made the viewport feel slow.
    void markDirty();
    /// Empty unless attachRenderer() failed, in which case it is the user-facing reason.
    [[nodiscard]] const QString& rendererError() const noexcept { return rendererError_; }

protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;

private:
    /// Projects a world point through the frame's view+projection to widget pixels. The same
    /// matrices the GPU uses, so if the camera is wrong here it is wrong there.
    [[nodiscard]] QPointF project(const float world[3], bool& visible) const;

    /// The wireframe reference, used when no GPU is attached.
    void paintFallback(QPainter&);

    /// Pushes the widget's size, in DEVICE pixels, down to the controller.
    void syncViewportSize();

    cad::app::Controller& controller_;
    QPoint lastMouse_;
    cad::render::Drag drag_ = cad::render::Drag::None;

    bool attached_ = false;
    QString rendererError_;

    /// The last rendered frame, reused until something actually changes it.
    QImage frame_;
    bool dirty_ = true;

    /// Retained across the paint because QImage does not copy the buffer it is constructed over,
    /// and the image must outlive drawImage().
    std::vector<std::uint8_t> pixels_;
};

}  // namespace cadqt
