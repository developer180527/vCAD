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
