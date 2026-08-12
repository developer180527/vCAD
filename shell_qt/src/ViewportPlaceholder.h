#pragma once

#include "cad/app/Controller.h"

#include <QWidget>

namespace cadqt {

/// Stands in for the bgfx viewport, and does real work while it does.
///
/// It draws the scene's bounding box and a ground grid in wireframe with QPainter, from the same
/// `SceneFrame` the GPU backend will consume. That is enough to prove the whole chain — document,
/// recompute, tessellation, scene assembly, camera — is wired correctly before any GPU code is
/// involved, and it means navigation can be built and felt now.
///
/// It also carries the mouse handling, so swapping in the real backend replaces `paintEvent` and
/// nothing else.
class ViewportPlaceholder : public QWidget {
    Q_OBJECT
public:
    explicit ViewportPlaceholder(cad::app::Controller& controller, QWidget* parent = nullptr);

protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;

private:
    /// Projects a world point through the frame's view+projection to widget pixels. The same
    /// matrices the GPU will use, so if the camera is wrong here it is wrong there.
    [[nodiscard]] QPointF project(const float world[3], bool& visible) const;

    cad::app::Controller& controller_;
    QPoint lastMouse_;
    cad::render::Drag drag_ = cad::render::Drag::None;
};

}  // namespace cadqt
