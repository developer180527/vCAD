#pragma once

#include "cad/render/Backend.h"
#include "cad/render/RenderMesh.h"

namespace cad::render {

/// Which mouse gesture does what. People are genuinely religious about this — a CAD user who
/// has middle-drag-orbits in their fingers will find any other mapping actively unusable — and
/// offering the three common conventions costs almost nothing.
enum class NavigationPreset : std::uint8_t {
    Cad,          ///< middle drag orbits, shift+middle pans. SolidWorks/Inventor.
    Fusion,       ///< middle drag pans, shift+middle orbits.
    Blender,      ///< middle drag orbits, shift+middle pans, but with roll-free turntable.
};

enum class Drag : std::uint8_t { None, Orbit, Pan, Zoom };

/// Turntable camera controller. Named `CameraController`, not `Camera`, because `Camera` in
/// Backend.h is the POD the backend consumes — this is the stateful thing that produces one.
///
/// Not a free-look camera: CAD users navigate around a part, and a turntable's "up is up"
/// invariant is what stops the view from rolling into confusion.
///
/// Distance/target rather than a matrix, because zoom-to-fit, orbit and pan are all natural in
/// those terms and awkward in a matrix.
class CameraController {
public:
    /// Recomputes view and projection for the given viewport.
    [[nodiscard]] Camera matrices(const Viewport&) const;

    /// A world-space ray through a pixel, for turning a click into a point on a plane.
    ///
    /// The GPU pick buffer answers "what did I click on", which is the wrong question while
    /// sketching: most of a sketch is drawn over empty space, where the pick buffer holds nothing.
    /// This answers "where in the world is this pixel", which has an answer everywhere.
    ///
    /// Pixels are DEVICE pixels with the origin at the top-left, the same convention as `pickAt`
    /// and as Qt's mouse events — one convention for screen coordinates, so a caller never has to
    /// remember which of two this particular function wanted.
    ///
    /// Under an orthographic projection every ray shares the view direction and they differ only in
    /// origin; under perspective they share an origin and differ in direction. Callers do not have
    /// to care, which is the reason this returns both rather than a direction and a promise.
    struct Ray {
        float origin[3]{0, 0, 0};
        float direction[3]{0, 0, -1};   ///< unit length
    };
    [[nodiscard]] Ray rayThrough(float x, float y, const Viewport&) const;

    /// Where the camera looks and which way is up on screen. Unit vectors, mutually perpendicular.
    ///
    /// Exists because yaw/pitch cannot express every view. See `alignTo`.
    struct Basis {
        float forward[3]{0, 0, -1};   ///< eye toward target
        float up[3]{0, 0, 1};         ///< screen up
        float right[3]{1, 0, 0};      ///< screen right, forward x up
    };

    /// The current orientation, however it was arrived at.
    [[nodiscard]] Basis basis() const noexcept;

    /// Points the camera straight at a plane: face-on, with `up` upright on screen.
    ///
    /// This cannot be done by setting yaw and pitch, which is the whole reason it exists. A
    /// turntable with world Z up has two problems with a sketch plane. The common case -- a sketch
    /// on XY, looked at from above -- is pitch = 90 degrees exactly, which is the pole the orbit
    /// clamp deliberately keeps the camera away from because `up` degenerates there. And even off
    /// the pole, yaw/pitch has no roll, so it cannot put the sketch's u axis along the screen's x:
    /// the sketch would appear face-on but rotated, and every dimension the user reads would be
    /// tilted.
    ///
    /// So an aligned camera carries an EXPLICIT basis, used in place of yaw/pitch until something
    /// orbits. `up` need not be perpendicular to the normal or even normalised; it is projected and
    /// orthonormalised here, so a caller can hand over a sketch frame's v axis as-is.
    void alignTo(const float origin[3], const float normal[3], const float up[3]);

    /// Whether the camera is holding an explicit alignment rather than a turntable pose.
    [[nodiscard]] bool aligned() const noexcept { return aligned_; }

    /// Returns to the turntable, keeping the direction currently being looked along.
    ///
    /// Seeds yaw and pitch from the aligned basis rather than snapping back to whatever they were,
    /// because a view that jumps somewhere else the moment you touch the mouse is disorienting in a
    /// way that reads as a bug. Roll is lost -- a turntable has none -- and at the pole the pitch
    /// clamp moves the view by about a degree. Both are unavoidable and neither is a jump.
    void releaseAlignment() noexcept;

    void orbit(float dxPixels, float dyPixels);
    void pan(float dxPixels, float dyPixels, const Viewport&);
    void zoom(float ticks);

    /// Frames `b` with a small margin. The one navigation command every CAD user presses
    /// within ten seconds of opening a file.
    void fit(const Bounds& b, const Viewport&);

    /// Maps a gesture to an action under the active preset. Kept here rather than in the shell
    /// so every shell — Qt, SwiftUI — behaves identically without reimplementing the table.
    [[nodiscard]] Drag dragFor(int button, bool shift, bool ctrl) const;

    /// NDC depth convention, from `bgfx::getCaps()->homogeneousDepth`.
    ///
    /// false ([0,1]) for Metal, D3D and Vulkan; true ([-1,1]) for OpenGL. Getting this wrong
    /// depth-clips the entire scene and renders NOTHING, with no error and no warning — which is
    /// exactly what happened: hand-rolled matrices assumed the OpenGL range and Metal silently
    /// discarded every fragment. The backend sets this at init; the default matches Metal
    /// because that is what ships first.
    void setHomogeneousDepth(bool v) noexcept { homogeneousDepth_ = v; }

    void setOrthographic(bool v) noexcept { orthographic_ = v; }
    [[nodiscard]] bool orthographic() const noexcept { return orthographic_; }
    void setPreset(NavigationPreset p) noexcept { preset_ = p; }

    [[nodiscard]] float distance() const noexcept { return distance_; }
    [[nodiscard]] const float* target() const noexcept { return target_; }

private:
    /// Turntable pose, and the aligned basis that overrides it. Two representations rather than one
    /// general orientation: orbit, pan and fit are all natural in yaw/pitch/distance and the
    /// turntable's "up is up" invariant is what stops a CAD view rolling into confusion. Alignment
    /// is the exception that needs roll, not the new normal.
    bool aligned_ = false;
    Basis alignedBasis_;

    float target_[3]{0, 0, 0};
    float distance_ = 500.0f;
    float yaw_ = 0.6f;        ///< radians
    float pitch_ = 0.5f;
    /// Orthographic by default. Engineers check alignment in ortho, and perspective makes
    /// coincident faces ambiguous — the opposite default to a game engine.
    bool orthographic_ = true;
    bool homogeneousDepth_ = false;
    NavigationPreset preset_ = NavigationPreset::Cad;
    float orthoHeight_ = 500.0f;
};

}  // namespace cad::render
