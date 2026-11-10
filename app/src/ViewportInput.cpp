#include "cad/app/ViewportInput.h"

#include "cad/app/Controller.h"

#include <algorithm>
#include <cmath>

namespace cad::app {

ViewportInput::ViewportInput(Controller& controller) : controller_(controller) {}

void ViewportInput::setScale(float dpr) { scale_ = dpr > 0 ? dpr : 1.0f; }

void ViewportInput::setSize(float w, float h) {
    width_ = std::max(1.0f, w);
    height_ = std::max(1.0f, h);
}

// DEVICE pixels for everything the id buffer answers. Logical coordinates there pick at half the
// intended position on a Retina display -- a bug that looks like an inaccurate picker rather than
// a units mistake. Controller::pickAt documents the same requirement.
std::uint32_t ViewportInput::device(float logical) const noexcept {
    return static_cast<std::uint32_t>(std::max(0.0f, logical * scale_));
}

std::uint32_t ViewportInput::aperture() const noexcept {
    return static_cast<std::uint32_t>(std::lround(kApertureLogical * scale_));
}

ViewportInput::Outcome ViewportInput::press(Button button, const Pointer& p) {
    // FIRST, before every early return: the flag may only ever affect the release of the gesture
    // that set it. Left stranded, the NEXT ordinary click is silently ignored.
    swallowRelease_ = false;
    lastX_ = pressX_ = p.x;
    lastY_ = pressY_ = p.y;
    dragged_ = false;

    // Orbit mode first. It is a mode the user turned on deliberately, so it beats the drawing tool
    // and the left-is-selection rule alike. leftPressDraws() already answers false in orbit mode
    // (tests/acceptance/navigation_gestures.cpp); checking here as well keeps the order readable.
    if (button == Button::Primary && controller_.orbitMode()) {
        drag_ = render::Drag::Orbit;
        return {};
    }

    // While a sketch tool is active the left button draws, as in every CAD sketcher. Alt still
    // navigates, and so do the middle button and the presets.
    if (button == Button::Primary && controller_.leftPressDraws() && !p.alt) {
        drag_ = render::Drag::None;
        Outcome out;
        out.action = Action::Drew;
        out.redraw = controller_.sketchClickAt(p.x * scale_, p.y * scale_);
        return out;
    }

    // The preset table is the camera's, so every shell maps buttons identically.
    const int index = button == Button::Primary ? 0 : button == Button::Middle ? 1 : 2;
    drag_ = controller_.camera().dragFor(index, p.shift, p.ctrl, p.alt);
    return {};
}

ViewportInput::Outcome ViewportInput::move(const Pointer& p) {
    Outcome out;
    if (drag_ == render::Drag::None) {
        // A half-drawn shape follows the pointer with no button held: the rubber band.
        if (controller_.environment() == Environment::Sketch && controller_.sketchPending().has_value()) {
            out.action = Action::Hovered;
            out.redraw = controller_.sketchHoverAt(p.x * scale_, p.y * scale_);
            return out;
        }
        // Pre-highlight, with the SAME aperture as the click: a hover answered by a different rule
        // than the click it predicts is not an answer.
        if (controller_.hoverAt(device(p.x), device(p.y), aperture())) {
            out.action = Action::Hovered;
            out.redraw = true;
        }
        return out;
    }

    if (std::fabs(p.x - pressX_) + std::fabs(p.y - pressY_) > kDragThresholdLogical) dragged_ = true;
    const float dx = p.x - lastX_, dy = p.y - lastY_;
    lastX_ = p.x;
    lastY_ = p.y;
    if (dx == 0 && dy == 0) return out;

    switch (drag_) {
        // Through the controller, which refuses while a sketch is open: a model rule.
        case render::Drag::Orbit: controller_.orbitCamera(dx, dy); break;
        case render::Drag::Pan:
            controller_.camera().pan(dx, dy, render::Viewport{std::uint32_t(width_), std::uint32_t(height_), 1.0f});
            break;
        case render::Drag::Zoom: controller_.camera().zoom(-dy * 0.1f); break;
        case render::Drag::None: break;
    }
    // Required: mutating the camera does not move the scene's copy of its matrices.
    controller_.cameraChanged();
    out.action = Action::Navigated;
    out.redraw = true;
    return out;
}

ViewportInput::Outcome ViewportInput::release(Button button, const Pointer& p) {
    const render::Drag was = drag_;
    drag_ = render::Drag::None;

    // A gesture that actually moved is not a click. One that never passed the threshold is, even
    // if a navigation drag was mapped to the button.
    if (was != render::Drag::None && dragged_) return {};
    if (button != Button::Primary) return {};

    // The trailing half of a double click, consumed here: it would re-select at the current level
    // and replace the body the double click just chose with the face under the pointer.
    if (swallowRelease_) {
        swallowRelease_ = false;
        return {};
    }
    // NOTE, kept as shell_qt had it: a press the drawing tool took is followed by an ordinary pick
    // at the same point. Moved, not changed -- whether a sketch click should also select is a
    // question for the sketcher, settled with a test, not in a refactor.

    Outcome out;
    // Choosing a sketch plane takes the click before selection sees it: Start Sketch with nothing
    // selected asks for a plane rather than guessing one (MODELLING_UX.md section 2).
    if (controller_.awaitingSketchPlane()) {
        controller_.sketchOnPickedPlane(device(p.x), device(p.y));
        out.action = Action::PickedPlane;
        out.redraw = true;
        return out;
    }
    const auto result = controller_.tapAt(device(p.x), device(p.y), aperture(), p.shift);
    out.action = Action::Selected;
    out.redraw = result.changed;
    out.message = result.message;
    return out;
}

ViewportInput::Outcome ViewportInput::doubleClick(Button button, const Pointer& p) {
    if (button != Button::Primary) return {};
    Outcome out;
    if (controller_.leftPressDraws()) {  // false in orbit mode: the Controller folds that in
        controller_.endSketchChain();
        // Without this the release after it runs an ordinary selection where the chain ended.
        swallowRelease_ = true;
        out.action = Action::EndedChain;
        out.redraw = true;
        return out;
    }
    if (controller_.environment() == Environment::Sketch) return out;

    // A double click selects the whole BODY, whatever the level says -- the iPad's double tap, and
    // what makes Auto usable as a default. It REPLACES what the first click selected: with Shift
    // held, adding instead put the face and its body in the selection side by side.
    controller_.clearSelection();
    const auto result =
        controller_.tapAt(device(p.x), device(p.y), aperture(), p.shift, Controller::SelectionLevel::Body);
    swallowRelease_ = true;
    out.action = Action::Selected;
    out.redraw = true;
    out.message = result.message;
    return out;
}

ViewportInput::Outcome ViewportInput::wheel(float notches) {
    if (notches == 0) return {};
    controller_.camera().zoom(notches);
    controller_.cameraChanged();
    return Outcome{Action::Navigated, true, {}};
}

ViewportInput::Outcome ViewportInput::leave() {
    Outcome out;
    if (drag_ != render::Drag::None) return out;  // a drag carries on outside; the release ends it
    if (controller_.clearHover()) {
        out.action = Action::Hovered;
        out.redraw = true;
    }
    return out;
}

}  // namespace cad::app
