#include "cad/render/Camera.h"

#include <bx/math.h>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace cad::render {
namespace {

// No hand-rolled projection maths.
//
// The previous version built lookAt/ortho/perspective by hand and assumed OpenGL's [-1,1] NDC
// depth. Metal, D3D and Vulkan use [0,1], so every fragment fell outside the depth range and the
// scene rendered EMPTY — no error, no warning, nothing on screen. bx::mtxOrtho and bx::mtxProj
// take the convention as a flag precisely because this is easy to get wrong and impossible to
// see. Use them.
//
// Handedness is RIGHT for both view and projection, and must stay matched. Mixing them is the other
// silent way to get a blank or inside-out scene.
//
// bx defaults to LEFT, and this file used the default for a long time. That is wrong for this world:
// vCAD is right-handed with Z up, and rendering a right-handed world through a left-handed view and
// projection produces a MIRRORED image. A front view put +X on the left of the screen -- the wrong
// side for every engineering drawing convention there is.
//
// It went unnoticed because nothing on screen could show it. Backface culling is deliberately off
// (BgfxBackend.cpp, "No backface culling, deliberately"), so a mirrored winding does not blank
// anything, and a box and a cylinder are symmetric. It surfaced the moment the camera was asked to
// align to a sketch frame, because a frame has a handedness: u x v = n, and u kept landing on the
// left. Sketching on a face is exactly where a mirrored viewport stops being invisible.
//
// Consequence to remember when reading matrices: in a right-handed view space, the camera looks
// down -z, so the third column of the view matrix is BACKWARD and points in front of the camera
// have NEGATIVE view-space z.

/// Vector helpers, local because three of them are used once each and a maths library for that
/// would be a dependency for its own sake.
void normalise(float v[3]) {
    const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (len < 1e-9f) return;
    v[0] /= len; v[1] /= len; v[2] /= len;
}

void crossInto(float out[3], const float a[3], const float b[3]) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

float dot3(const float a[3], const float b[3]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

}  // namespace

CameraController::Basis CameraController::basis() const noexcept {
    if (aligned_) return alignedBasis_;

    Basis out;
    // The turntable, expressed in the same terms. `d` is target -> eye, so forward is its negation.
    const float cp = std::cos(pitch_);
    const float d[3]{cp * std::cos(yaw_), cp * std::sin(yaw_), std::sin(pitch_)};
    for (int i = 0; i < 3; ++i) out.forward[i] = -d[i];

    constexpr float kWorldUp[3]{0, 0, 1};
    crossInto(out.right, out.forward, kWorldUp);
    normalise(out.right);
    crossInto(out.up, out.right, out.forward);
    normalise(out.up);
    return out;
}

void CameraController::alignTo(const float origin[3], const float normal[3], const float up[3]) {
    for (int i = 0; i < 3; ++i) target_[i] = origin[i];

    Basis b;
    // Looked at FROM the +normal side, so forward is against the normal. The other choice would
    // show the user the back of the face they just clicked.
    for (int i = 0; i < 3; ++i) b.forward[i] = -normal[i];
    normalise(b.forward);

    // Gram-Schmidt: whatever component of `up` runs along the view direction cannot be shown, so
    // remove it. This is why a caller may pass a sketch frame's v axis unmodified.
    const float along = dot3(up, b.forward);
    for (int i = 0; i < 3; ++i) b.up[i] = up[i] - along * b.forward[i];

    if (std::sqrt(dot3(b.up, b.up)) < 1e-6f) {
        // `up` was parallel to the view direction, so it says nothing about screen orientation.
        // Any perpendicular will do rather than leaving a zero vector, which would make the view
        // matrix singular and blank the viewport with no error -- the same class of silent failure
        // as the NDC depth mistake this file's header comment records.
        const float fallback[3]{0, 0, 1};
        const float other[3]{1, 0, 0};
        const bool nearlyVertical = std::abs(b.forward[2]) > 0.9f;
        crossInto(b.up, b.forward, nearlyVertical ? other : fallback);
    }
    normalise(b.up);
    crossInto(b.right, b.forward, b.up);
    normalise(b.right);

    aligned_ = true;
    alignedBasis_ = b;
}

void CameraController::releaseAlignment() noexcept {
    if (!aligned_) return;
    const Basis b = alignedBasis_;
    aligned_ = false;

    // target -> eye, which is what yaw and pitch describe.
    const float d[3]{-b.forward[0], -b.forward[1], -b.forward[2]};
    pitch_ = std::asin(std::clamp(d[2], -1.0f, 1.0f));

    if (std::abs(d[2]) > 0.999f) {
        // Straight down or straight up: `d` has no horizontal component, so atan2 on it is
        // meaningless (atan2(0,0) is 0) and every plan view would swing east regardless of how the
        // sketch was oriented. The screen-up vector still carries the heading, so read it there. At
        // pitch = +90 a turntable's screen up is (-cos yaw, -sin yaw, 0); at -90 it is the negation.
        yaw_ = d[2] > 0.0f ? std::atan2(-b.up[1], -b.up[0]) : std::atan2(b.up[1], b.up[0]);
    } else {
        yaw_ = std::atan2(d[1], d[0]);
    }

    constexpr float kLimit = 1.5533f;   // ~89 degrees, matching orbit()
    pitch_ = std::clamp(pitch_, -kLimit, kLimit);
}

render::Camera CameraController::matrices(const Viewport& vp) const {
    Camera out;
    out.orthographic = orthographic_;

    // One path for both poses. The turntable expresses itself as a basis (see basis()) rather than
    // matrices() branching on alignment, so there is exactly one place that builds a view matrix --
    // and the Z-up convention below lives in exactly one place too.
    //
    // Z-up. CAD convention, unlike most game engines' Y-up: STEP, drawings and every manufacturing
    // workflow treat Z as vertical, and fighting it makes every imported part arrive lying on its
    // side. An ALIGNED camera is the deliberate exception: a sketch on a vertical face has the
    // face's own up, which is the point of aligning to it.
    const Basis b = basis();
    const bx::Vec3 eye{target_[0] - distance_ * b.forward[0],
                       target_[1] - distance_ * b.forward[1],
                       target_[2] - distance_ * b.forward[2]};
    const bx::Vec3 at{target_[0], target_[1], target_[2]};
    const bx::Vec3 up{b.up[0], b.up[1], b.up[2]};

    out.eye[0] = eye.x;
    out.eye[1] = eye.y;
    out.eye[2] = eye.z;
    bx::mtxLookAt(out.view.m, eye, at, up, bx::Handedness::Right);

    const float aspect = vp.height == 0
        ? 1.0f
        : static_cast<float>(vp.width) / static_cast<float>(vp.height);

    // Depth range scaled to the scene rather than fixed. A fixed near plane at 0.1 with a
    // 10-metre assembly throws away most of the depth buffer's precision and produces
    // z-fighting on coincident faces — which CAD models are full of.
    const float zn = std::max(distance_ * 0.001f, 1e-4f);
    const float zf = std::max(distance_ * 10.0f, zn * 100.0f);

    if (orthographic_) {
        const float halfH = orthoHeight_ * 0.5f;
        const float halfW = halfH * aspect;
        // Symmetric depth range for ortho: a CAD user rotating a part must not have it clip
        // through the near plane, and there is no perspective cost to being generous.
        bx::mtxOrtho(out.projection.m, -halfW, halfW, -halfH, halfH, -zf, zf, 0.0f,
                     homogeneousDepth_, bx::Handedness::Right);
    } else {
        bx::mtxProj(out.projection.m, bx::toDeg(0.7f), aspect, zn, zf, homogeneousDepth_,
                    bx::Handedness::Right);
    }
    return out;
}

void CameraController::orbit(float dx, float dy) {
    // Orbiting IS the request to leave a face-on view, so the alignment is released rather than
    // ignored -- and released to the direction currently being looked along, so the first pixel of
    // drag does not teleport the camera back to a pose from before the sketch was opened.
    releaseAlignment();

    constexpr float kRadiansPerPixel = 0.008f;
    yaw_ -= dx * kRadiansPerPixel;
    // Clamped just shy of the poles. Reaching them makes `up` degenerate and the view flips —
    // the classic turntable bug, and jarring enough that users report it as a crash.
    constexpr float kLimit = 1.5533f;   // ~89 degrees
    pitch_ = std::clamp(pitch_ - dy * kRadiansPerPixel, -kLimit, kLimit);
}

void CameraController::pan(float dx, float dy, const Viewport& vp) {
    if (vp.height == 0) return;
    // World units per pixel, so panning tracks the pointer exactly at any zoom. Scaling by a
    // constant instead is what makes a viewport feel like it is fighting you.
    const float worldPerPixel = (orthographic_ ? orthoHeight_ : distance_)
                                / static_cast<float>(vp.height);

    // Through basis() so panning follows the ALIGNED orientation too. Recomputing the turntable
    // vectors here instead would drag a face-on sketch view along the world's axes rather than the
    // screen's -- the pointer and the model moving in different directions, which feels broken long
    // before anyone works out why.
    const Basis b = basis();
    for (int i = 0; i < 3; ++i) {
        target_[i] += (-b.right[i] * dx + b.up[i] * dy) * worldPerPixel;
    }
}

void CameraController::zoom(float ticks) {
    // Multiplicative, so each tick feels the same whether you are looking at a bolt or a
    // building. Additive zoom crawls when far and overshoots when near.
    const float factor = std::pow(1.1f, -ticks);
    distance_ = std::clamp(distance_ * factor, 1e-3f, 1e9f);
    orthoHeight_ = std::clamp(orthoHeight_ * factor, 1e-3f, 1e9f);
}

void CameraController::fit(const Bounds& b, const Viewport& vp) {
    if (!b.valid()) return;
    for (int i = 0; i < 3; ++i) target_[i] = (b.min[i] + b.max[i]) * 0.5f;

    const float extent[3]{b.max[0] - b.min[0], b.max[1] - b.min[1], b.max[2] - b.min[2]};
    const float radius = 0.5f * std::sqrt(extent[0] * extent[0] + extent[1] * extent[1]
                                          + extent[2] * extent[2]);
    // A degenerate bound (a single point, an empty document) must still produce a usable
    // camera rather than distance 0 and a division by zero downstream.
    const float safe = std::max(radius, 1e-3f);

    constexpr float kMargin = 1.15f;
    orthoHeight_ = safe * 2.0f * kMargin;
    distance_ = safe * 3.0f;
    (void)vp;   // aspect is applied in matrices(); fit is aspect-independent by design
}

Drag CameraController::dragFor(int button, bool shift, bool ctrl) const {
    constexpr int kLeft = 0, kMiddle = 1, kRight = 2;
    if (ctrl && button == kMiddle) return Drag::Zoom;

    switch (preset_) {
        case NavigationPreset::Cad:
        case NavigationPreset::Blender:
            if (button == kMiddle) return shift ? Drag::Pan : Drag::Orbit;
            if (button == kRight) return Drag::Pan;
            break;
        case NavigationPreset::Fusion:
            // Deliberately inverted: Fusion users expect middle-drag to pan, and the muscle
            // memory is strong enough that getting it wrong reads as a broken viewport.
            if (button == kMiddle) return shift ? Drag::Orbit : Drag::Pan;
            if (button == kRight) return Drag::Orbit;
            break;
    }
    if (button == kLeft) return Drag::None;   // left is selection, never navigation
    return Drag::None;
}

}  // namespace cad::render
