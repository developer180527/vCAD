#include "cad/render/Camera.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace cad::render {
namespace {

void lookAt(const float eye[3], const float at[3], float out[16]) {
    float f[3]{at[0] - eye[0], at[1] - eye[1], at[2] - eye[2]};
    const float fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    if (fl > 1e-9f) { f[0] /= fl; f[1] /= fl; f[2] /= fl; }

    // Z-up. CAD convention, unlike most game engines' Y-up: STEP, drawings and every
    // manufacturing workflow treat Z as vertical, and fighting that makes every imported part
    // arrive lying on its side.
    const float up[3]{0.0f, 0.0f, 1.0f};
    float s[3]{f[1] * up[2] - f[2] * up[1], f[2] * up[0] - f[0] * up[2],
               f[0] * up[1] - f[1] * up[0]};
    float sl = std::sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
    if (sl < 1e-6f) {
        // Looking straight down the up axis: pick any perpendicular rather than emit NaNs.
        s[0] = 1.0f; s[1] = 0.0f; s[2] = 0.0f; sl = 1.0f;
    }
    s[0] /= sl; s[1] /= sl; s[2] /= sl;
    const float u[3]{s[1] * f[2] - s[2] * f[1], s[2] * f[0] - s[0] * f[2],
                     s[0] * f[1] - s[1] * f[0]};

    out[0] = s[0];  out[4] = s[1];  out[8]  = s[2];
    out[1] = u[0];  out[5] = u[1];  out[9]  = u[2];
    out[2] = -f[0]; out[6] = -f[1]; out[10] = -f[2];
    out[3] = 0; out[7] = 0; out[11] = 0; out[15] = 1;
    out[12] = -(s[0] * eye[0] + s[1] * eye[1] + s[2] * eye[2]);
    out[13] = -(u[0] * eye[0] + u[1] * eye[1] + u[2] * eye[2]);
    out[14] = f[0] * eye[0] + f[1] * eye[1] + f[2] * eye[2];
}

void ortho(float halfH, float aspect, float zn, float zf, float out[16]) {
    const float halfW = halfH * aspect;
    std::fill(out, out + 16, 0.0f);
    out[0] = 1.0f / halfW;
    out[5] = 1.0f / halfH;
    out[10] = -2.0f / (zf - zn);
    out[14] = -(zf + zn) / (zf - zn);
    out[15] = 1.0f;
}

void perspective(float fovY, float aspect, float zn, float zf, float out[16]) {
    const float t = 1.0f / std::tan(fovY * 0.5f);
    std::fill(out, out + 16, 0.0f);
    out[0] = t / aspect;
    out[5] = t;
    out[10] = (zf + zn) / (zn - zf);
    out[11] = -1.0f;
    out[14] = (2.0f * zf * zn) / (zn - zf);
}

}  // namespace

render::Camera CameraController::matrices(const Viewport& vp) const {
    Camera out;
    out.orthographic = orthographic_;

    const float cp = std::cos(pitch_);
    const float eye[3]{target_[0] + distance_ * cp * std::cos(yaw_),
                       target_[1] + distance_ * cp * std::sin(yaw_),
                       target_[2] + distance_ * std::sin(pitch_)};
    for (int i = 0; i < 3; ++i) out.eye[i] = eye[i];
    lookAt(eye, target_, out.view.m);

    const float aspect = vp.height == 0
        ? 1.0f
        : static_cast<float>(vp.width) / static_cast<float>(vp.height);

    // Depth range scaled to the scene rather than fixed. A fixed near plane at 0.1 with a
    // 10-metre assembly throws away most of the depth buffer's precision and produces
    // z-fighting on coincident faces — which CAD models are full of.
    const float zn = std::max(distance_ * 0.001f, 1e-4f);
    const float zf = std::max(distance_ * 10.0f, zn * 100.0f);

    if (orthographic_) {
        ortho(orthoHeight_ * 0.5f, aspect, -zf, zf, out.projection.m);
    } else {
        perspective(0.7f, aspect, zn, zf, out.projection.m);
    }
    return out;
}

void CameraController::orbit(float dx, float dy) {
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

    const float cp = std::cos(pitch_);
    const float f[3]{cp * std::cos(yaw_), cp * std::sin(yaw_), std::sin(pitch_)};
    const float up[3]{0, 0, 1};
    float right[3]{f[1] * up[2] - f[2] * up[1], f[2] * up[0] - f[0] * up[2],
                   f[0] * up[1] - f[1] * up[0]};
    const float rl = std::sqrt(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
    if (rl > 1e-6f) { right[0] /= rl; right[1] /= rl; right[2] /= rl; }
    const float camUp[3]{right[1] * f[2] - right[2] * f[1], right[2] * f[0] - right[0] * f[2],
                         right[0] * f[1] - right[1] * f[0]};

    for (int i = 0; i < 3; ++i) {
        target_[i] += (-right[i] * dx + camUp[i] * dy) * worldPerPixel;
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
