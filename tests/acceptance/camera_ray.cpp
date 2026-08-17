/// A pixel maps back to the world the same way the picture was drawn.
///
/// # Why this is a round trip and not a table of expected numbers
///
/// `rayThrough` is hand-rolled basis arithmetic; `matrices()` builds its view and projection with
/// bx's matrix routines. They are two independent implementations of the same camera, which is
/// exactly what makes checking one against the other worth doing — a table of expected coordinates
/// would only ever agree with whichever of the two I wrote it from.
///
/// So: take a world point, project it through the REAL matrices to a pixel, fire a ray back through
/// that pixel, and require the ray to pass through the point it started as. A sign error, a
/// transposed basis, a missing Y flip or an aspect applied to the wrong axis all break this and
/// none of them break a test that only checks a ray is unit length.

#include "cad/render/Camera.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cmath>

using namespace cad;

namespace {

/// Projects a world point with the camera's own matrices, returning a device pixel.
///
/// Deliberately uses `Camera::view` and `Camera::projection` rather than reimplementing the
/// transform: the point of this file is that the ray agrees with the MATRICES THAT DREW THE FRAME.
struct Pixel {
    float x = 0.0f;
    float y = 0.0f;
    bool visible = false;
};

Pixel project(const render::Camera& cam, const render::Viewport& vp, const float world[3]) {
    const auto mul = [](const render::Mat4& m, const float in[4], float out[4]) {
        for (int r = 0; r < 4; ++r) {
            out[r] = m.m[r] * in[0] + m.m[4 + r] * in[1] + m.m[8 + r] * in[2] + m.m[12 + r] * in[3];
        }
    };
    const float point[4]{world[0], world[1], world[2], 1.0f};
    float eye[4];
    float clip[4];
    mul(cam.view, point, eye);
    mul(cam.projection, eye, clip);
    if (std::abs(clip[3]) < 1e-9f) return {};

    const float ndcX = clip[0] / clip[3];
    const float ndcY = clip[1] / clip[3];
    Pixel out;
    out.x = (ndcX * 0.5f + 0.5f) * static_cast<float>(vp.width);
    out.y = (0.5f - ndcY * 0.5f) * static_cast<float>(vp.height);
    out.visible = true;
    return out;
}

/// Perpendicular distance from a point to a ray. Zero when the ray passes through the point.
double distanceToRay(const render::CameraController::Ray& ray, const float p[3]) {
    const double dx = p[0] - ray.origin[0];
    const double dy = p[1] - ray.origin[1];
    const double dz = p[2] - ray.origin[2];
    const double along = dx * ray.direction[0] + dy * ray.direction[1] + dz * ray.direction[2];
    const double cx = dx - along * ray.direction[0];
    const double cy = dy - along * ray.direction[1];
    const double cz = dz - along * ray.direction[2];
    return std::sqrt(cx * cx + cy * cy + cz * cz);
}

}  // namespace

TEST_CASE("a ray through a pixel passes through the point that projected there", "[camera][ray]") {
    const bool orthographic = GENERATE(true, false);

    render::Viewport vp;
    vp.width = 1280;
    vp.height = 800;   // deliberately not square, so an aspect applied to the wrong axis shows up

    render::CameraController camera;
    camera.setOrthographic(orthographic);
    // Off-axis on purpose. An axis-aligned camera hides sign and basis errors, because several
    // wrong answers coincide with the right one when two of the three components are zero.
    camera.orbit(37.0f, 22.0f);

    const float points[][3]{
        {0.0f, 0.0f, 0.0f},
        {40.0f, 0.0f, 0.0f},
        {0.0f, 30.0f, 0.0f},
        {0.0f, 0.0f, 20.0f},
        {-25.0f, 18.0f, -9.0f},
    };

    const render::Camera cam = camera.matrices(vp);
    for (const auto& p : points) {
        const Pixel pixel = project(cam, vp, p);
        REQUIRE(pixel.visible);

        const auto ray = camera.rayThrough(pixel.x, pixel.y, vp);

        // Unit length, because every caller scales it by a distance along the ray.
        const double length = std::sqrt(ray.direction[0] * ray.direction[0] +
                                        ray.direction[1] * ray.direction[1] +
                                        ray.direction[2] * ray.direction[2]);
        CHECK_THAT(length, Catch::Matchers::WithinAbs(1.0, 1e-4));

        // The assertion that matters. Tolerance in world units, generous enough for float matrix
        // arithmetic at this scale and far tighter than any of the mistakes it is looking for —
        // a Y flip on this camera moves the answer by tens of millimetres.
        CHECK(distanceToRay(ray, p) < 0.05);
    }
}

TEST_CASE("the centre pixel looks straight down the view direction", "[camera][ray]") {
    render::Viewport vp;
    vp.width = 900;
    vp.height = 600;

    render::CameraController camera;
    camera.orbit(15.0f, -40.0f);

    const auto ray = camera.rayThrough(450.0f, 300.0f, vp);
    const auto basis = camera.basis();

    // Independent of the round trip above: whatever else is true, the middle of the screen is the
    // direction the camera is pointing. This is the check that still means something if `project`
    // above and `rayThrough` ever shared a mistake.
    for (int i = 0; i < 3; ++i) {
        CHECK_THAT(static_cast<double>(ray.direction[i]),
                   Catch::Matchers::WithinAbs(static_cast<double>(basis.forward[i]), 1e-4));
    }
}
