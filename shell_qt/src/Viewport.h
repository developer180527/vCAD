#pragma once

#include "cad/app/Controller.h"

#include <QImage>
#include <QString>
#include <QLabel>
#include <QWidget>

#include <cstdint>
#include <vector>

class QLabel;
class QPainter;
class QPaintEngine;

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

    /// Makes every viewport use the offscreen-and-blit path, whatever the platform supports.
    ///
    /// Set by `--shot`. A natively-presenting viewport hands its pixels to a CAMetalLayer, and
    /// QWidget::grab() cannot capture that: paintEngine() returns null precisely so Qt does not
    /// draw the widget, so a grab renders the window with a hole where the model is. Worse, grab
    /// drives a nested synchronous paint, which is the same main-thread stall that deferring
    /// document creation into the event loop exists to avoid.
    ///
    /// The consequence is worth stating plainly: screenshots therefore never exercise the native
    /// path. They verify the shell around the viewport, not the presentation route the user gets.
    static void setForceOffscreen(bool) noexcept;

    [[nodiscard]] bool rendererAttached() const noexcept { return attached_; }

    /// Marks the rendered frame stale. Repainting does NOT re-render by itself: an offscreen
    /// frame costs a GPU readback, and Qt repaints a widget for many reasons that have nothing
    /// to do with the scene — a ribbon hover, a tooltip, an overlapping panel. Re-rendering on
    /// each of those is most of what made the viewport feel slow.
    void markDirty();

    /// Places the dimension field for `--shot`, which has no pointer to follow.
    void syncDimensionFieldForShot() { syncDimensionField(); }

    /// The dimension labels currently on screen, for the wiring probe.
    ///
    /// They are separate top-level windows -- the only way to paint over the Metal layer -- which
    /// means a window grab cannot see them and a screenshot cannot prove they are there. This can.
    [[nodiscard]] std::vector<QString> visibleDimensionLabelsForProbe() const {
        std::vector<QString> out;
        for (const QLabel* label : dimensionLabels_) {
            if (label->isVisible()) out.push_back(label->text());
        }
        return out;
    }

signals:
    /// Why a click did not select, when there is a reason worth saying — a curved face, geometry
    /// with no name, empty space. Emitted rather than swallowed: a click that does nothing and
    /// explains nothing is the failure this whole path was reported as.
    void pickMessage(const QString& text);

    /// The live dimension changed. Separate from the document's own change signal because a hover
    /// moves the numbers without touching the sketch, and the status bar shows them too.
    void dimensionChanged();

public:
    /// Empty unless attachRenderer() failed, in which case it is the user-facing reason.
    [[nodiscard]] const QString& rendererError() const noexcept { return rendererError_; }

    /// Null while presenting to a native surface, which is how a widget tells Qt "I own these
    /// pixels, do not bring a paint engine". Without it Qt paints its own background over the
    /// Metal layer and the viewport flickers between the two.
    [[nodiscard]] QPaintEngine* paintEngine() const override;

protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    /// Digits typed while a shape is half-drawn are its dimension, not shortcuts.
    void keyPressEvent(QKeyEvent*) override;

    /// Ends a chain of segments. Qt delivers press, release, then a DOUBLE-CLICK in place of the
    /// second press — so without this the second click never reaches the controller and a chain
    /// cannot be ended with the mouse alone.
    void mouseDoubleClickEvent(QMouseEvent*) override;

    /// Both hide the floating dimension readout: a top-level window does not vanish with its owner,
    /// and one left showing floats over whatever the user switches to.
    void hideEvent(QHideEvent*) override;
    void leaveEvent(QEvent*) override;

    /// Stops Qt stealing Tab for focus navigation while a dimension is being entered.
    ///
    /// Qt consumes Tab BEFORE keyPressEvent runs, so a Tab-to-lock handler there never fires — the
    /// key silently moves focus to the next widget instead, which looks like the shortcut simply
    /// not working. Refused only while sketching, so Tab still walks the UI everywhere else.
    bool focusNextPrevChild(bool next) override;

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

    /// Where the press landed, and whether the mouse has moved far enough since to count as a drag.
    ///
    /// A click and an orbit start identically, so the two can only be told apart on RELEASE. Without
    /// the threshold, the hand-tremor of a normal click registers as a drag and the click is lost —
    /// which is indistinguishable from picking not working at all.
    QPoint pressAt_;
    bool dragged_ = false;

    bool attached_ = false;
    /// Presenting straight to a native surface, rather than blitting a read-back image.
    bool native_ = false;
    QString rendererError_;

    /// Moves the live dimension field to the cursor and fills it in, or hides it.
    void syncDimensionField();

    /// The live dimension readout, a CHILD of the viewport rather than something painted into the
    /// frame.
    ///
    /// Painting it would need a text renderer on the GPU: while presenting to a native surface the
    /// widget owns its pixels and Qt does not draw into them, which is the same constraint that
    /// keeps the sketch overlay in the scene rather than in a QPainter pass. A child widget
    /// composites over the surface instead, and costs no font atlas.
    QLabel* dimensionField_ = nullptr;

    /// One label per dimension in the sketch, pooled and reused.
    ///
    /// Separate windows rather than one overlay covering the viewport, for the reason the header
    /// comment beside `dimensionField_` records: a child widget over the Metal layer paints into a
    /// surface nothing ever clears, and a full-viewport window would also have to be made
    /// click-through. Labels are cheap and there are a handful per sketch.
    std::vector<QLabel*> dimensionLabels_;
    void syncDimensionLabels();

    /// The last rendered frame, reused until something actually changes it.
    QImage frame_;
    bool dirty_ = true;

    /// Retained across the paint because QImage does not copy the buffer it is constructed over,
    /// and the image must outlive drawImage().
    std::vector<std::uint8_t> pixels_;
};

}  // namespace cadqt
