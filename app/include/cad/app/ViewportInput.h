#pragma once

/// What a pointer does in the 3D viewport: the gesture rules, once, for every shell.
///
/// # Why this exists
///
/// shell_qt's Viewport.cpp and shell_libgui's Shell.cpp each carried these rules, and the second
/// was a transcription of the first ("the rules mirror shell_qt's Viewport.cpp"). The rules are
/// decisions about the model, not about a toolkit: in a sketch a left press draws; orbit mode
/// beats the button presets; four logical pixels separate a click from a drag; the release after a
/// double click is swallowed or it re-selects at the wrong level; a click waiting for a sketch
/// plane picks the plane. Each of those was a bug before it was a rule, and a third shell (the
/// iPad, a Rust one) copying them would reintroduce whichever it missed.
///
/// So a shell translates its toolkit's events into the calls below -- logical pixels, top-left
/// origin, the button and the modifiers -- and this decides. It calls the Controller itself and
/// returns what it did, which is what a shell needs to repaint and to show a message, and what a
/// test needs to check a rule without a GPU.
///
/// # What is not here
///
/// The keyboard (the sketch dimension field, Escape), and where a shell draws its readouts. Both
/// are the next things to move; neither is a pointer rule.

#include <cstdint>
#include <string>

#include "cad/render/Camera.h"

namespace cad::app {

class Controller;

class ViewportInput {
public:
    enum class Button : std::uint8_t { Primary, Middle, Secondary };

    /// Where the pointer is, in the viewport's LOGICAL pixels, and which modifiers are held.
    struct Pointer {
        float x = 0, y = 0;
        bool shift = false, ctrl = false, alt = false;
    };

    /// What an event did.
    enum class Action : std::uint8_t {
        None,
        Drew,          ///< a sketch tool took the press
        Navigated,     ///< orbit, pan or zoom moved the camera
        Hovered,       ///< the pre-highlight changed
        Selected,      ///< a click picked, or tried to (see `message`)
        PickedPlane,   ///< the click answered "which plane to sketch on"
        EndedChain,    ///< a double click finished a run of sketch segments
    };

    struct Outcome {
        Action action = Action::None;
        bool redraw = false;      ///< something on screen changed
        std::string message;      ///< for the status line; empty when there is nothing to say
    };

    /// A click selects within this many LOGICAL pixels: a one-pixel edge must not be a test of
    /// motor control (docs/design/SELECTION.md). Scaled to device pixels by the device pixel ratio.
    static constexpr float kApertureLogical = 4.0f;

    /// Movement below this, in logical pixels (Manhattan), is a click with a shaky hand rather
    /// than a drag. Treated as a drag it both fails to select and nudges the camera.
    static constexpr float kDragThresholdLogical = 4.0f;

    explicit ViewportInput(Controller&);

    /// Device pixels per logical pixel, and the viewport's logical size (pan is scaled by it).
    void setScale(float devicePixelRatio);
    void setSize(float logicalWidth, float logicalHeight);

    Outcome press(Button, const Pointer&);
    Outcome move(const Pointer&);
    Outcome release(Button, const Pointer&);
    /// The second press of a double click. Toolkits differ on order -- Qt sends press, release,
    /// double-click, release; libgui reports the double click with the second release -- and
    /// either way the release that follows this is swallowed.
    Outcome doubleClick(Button, const Pointer&);
    /// Notches of wheel, positive away from the user.
    Outcome wheel(float notches);
    /// The pointer left the viewport: nothing is under it any more.
    Outcome leave();

    /// The gesture in progress, if any.
    [[nodiscard]] render::Drag drag() const noexcept { return drag_; }

private:
    [[nodiscard]] std::uint32_t device(float logical) const noexcept;
    [[nodiscard]] std::uint32_t aperture() const noexcept;

    Controller& controller_;
    float scale_ = 1.0f;
    float width_ = 1.0f, height_ = 1.0f;

    render::Drag drag_ = render::Drag::None;
    bool dragged_ = false;
    bool swallowRelease_ = false;
    float pressX_ = 0, pressY_ = 0, lastX_ = 0, lastY_ = 0;
};

}  // namespace cad::app
