#pragma once

#include <QWidget>

namespace cad::app { class Controller; }

namespace cadqt {

/// The sketch editor's drawing surface.
///
/// A 2D canvas, deliberately separate from the 3D viewport rather than an overlay on it. A sketch is
/// edited on its plane, face-on, with its own coordinate system — and mixing that into the 3D view's
/// camera means every click has to be unprojected onto a plane before it means anything. Inventor
/// and SolidWorks both rotate the view flat to the sketch for the same reason.
///
/// Painted with QPainter, not the GPU. A sketch is tens of curves, not millions of triangles, and
/// this way the editor works today rather than waiting on the renderer.
class SketchCanvas : public QWidget {
    Q_OBJECT
public:
    explicit SketchCanvas(cad::app::Controller& controller, QWidget* parent = nullptr);

    /// Which curve the next click starts. Set by the ribbon's Draw panel.
    enum class Tool { Select, Line, Circle };
    void setTool(Tool);
    [[nodiscard]] Tool tool() const noexcept { return tool_; }

    /// Frames the sketch. Called when the environment is entered, since a sketch loaded from a file
    /// can be anywhere.
    void fit();

signals:
    /// Emitted after any edit, so the window can refresh the DOF readout.
    void sketchChanged();

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void resizeEvent(QResizeEvent*) override;

private:
    [[nodiscard]] QPointF toScreen(double u, double v) const;
    [[nodiscard]] std::pair<double, double> toSketch(QPointF) const;

    /// Nearest existing endpoint within the snap radius, in SKETCH units.
    ///
    /// Snapping is what makes a drawn profile closed rather than nearly closed. It returns the
    /// geometry and which of its points matched, so the caller can add a real Coincident constraint
    /// rather than merely placing the new point at the same coordinates — coordinates drift the
    /// moment anything solves, a constraint does not.
    struct Snap {
        bool found = false;
        std::uint32_t geo = 0;
        int point = 0;      ///< PointRef as int, to keep sketch types out of this header
        double u = 0.0;
        double v = 0.0;
    };
    [[nodiscard]] Snap snapAt(QPointF screen) const;

    /// Nearest curve under the cursor within the pick radius, or kNoGeo.
    ///
    /// Hit testing is VIEW work, not model work: it needs screen distances and a pixel tolerance,
    /// because "close enough to click" is a property of the display, not of the sketch. The
    /// resulting selection is handed to the Controller, which owns it.
    [[nodiscard]] std::uint32_t pickAt(QPointF screen) const;

    cad::app::Controller& controller_;
    Tool tool_ = Tool::Line;

    double scale_ = 3.0;         ///< screen pixels per sketch unit
    QPointF origin_{0.0, 0.0};   ///< sketch (0,0) in screen coordinates

    bool drawing_ = false;
    double startU_ = 0.0;
    double startV_ = 0.0;
    Snap startSnap_;
    QPointF cursor_;
};

}  // namespace cadqt
