/// A pixel becomes a point on the sketch plane — the rule in-place sketching is built on.
///
/// # Why this is tested here and not in the shell
///
/// "A click at these pixels means that point in the sketch" is a model rule, exactly like "a click
/// at these pixels means that face" was. Both shells must agree on it, and neither Qt nor a GPU is
/// needed to decide it — so it lives in `app/` with a test that runs headless, and the shell is left
/// with nothing to get wrong but the plumbing.
///
/// # The shape of the checks
///
/// Every case here projects a KNOWN sketch coordinate to a pixel through the camera's own matrices
/// and then asks `sketchPointAt` to bring it back. The two directions are independent code — bx
/// matrix arithmetic one way, plane intersection the other — so agreement means something. Checking
/// against hand-computed pixel numbers would only agree with whichever side I derived them from.

#include "cad/app/Controller.h"
#include "cad/sketch/Sketch.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

using namespace cad;

namespace {

/// The device pixel a world point projects to, using the camera that drew the frame.
std::array<float, 2> pixelOf(const render::Camera& cam, const render::Viewport& vp,
                             const std::array<double, 3>& world) {
    const auto mul = [](const render::Mat4& m, const float in[4], float out[4]) {
        for (int r = 0; r < 4; ++r) {
            out[r] = m.m[r] * in[0] + m.m[4 + r] * in[1] + m.m[8 + r] * in[2] + m.m[12 + r] * in[3];
        }
    };
    const float point[4]{static_cast<float>(world[0]), static_cast<float>(world[1]),
                         static_cast<float>(world[2]), 1.0f};
    float eye[4];
    float clip[4];
    mul(cam.view, point, eye);
    mul(cam.projection, eye, clip);
    return {(clip[0] / clip[3] * 0.5f + 0.5f) * static_cast<float>(vp.width),
            (0.5f - clip[1] / clip[3] * 0.5f) * static_cast<float>(vp.height)};
}

}  // namespace

TEST_CASE("a pixel maps back to the sketch coordinate that drew it", "[sketch][viewport]") {
    app::Controller controller;
    controller.setViewportSize(1280, 800);
    // Off-axis, so a transposed or mirrored mapping cannot coincide with the right answer.
    controller.camera().orbit(28.0f, 19.0f);

    const document::ObjectId id = controller.beginSketch();
    REQUIRE(id != document::ObjectId{});
    REQUIRE(controller.activeSketch() != nullptr);

    const render::Viewport vp{1280, 800, 1.0f};
    const render::Camera cam = controller.camera().matrices(vp);

    // A default sketch is on XY, so (u, v) is (x, y) at z = 0. Stated explicitly rather than read
    // back from the sketch: if this test asked the code under test where its own plane was, it
    // would agree with itself no matter what the plane actually is.
    const double samples[][2]{{0.0, 0.0}, {25.0, 0.0}, {0.0, -18.0}, {-12.5, 31.25}};
    for (const auto& s : samples) {
        const auto pixel = pixelOf(cam, vp, {s[0], s[1], 0.0});
        const auto back = controller.sketchPointAt(pixel[0], pixel[1]);
        REQUIRE(back);
        CHECK_THAT((*back)[0], Catch::Matchers::WithinAbs(s[0], 0.05));
        CHECK_THAT((*back)[1], Catch::Matchers::WithinAbs(s[1], 0.05));
    }
}

TEST_CASE("the mapping follows a tilted sketch frame, not the global axes", "[sketch][viewport]") {
    app::Controller controller;
    controller.setViewportSize(1024, 768);
    controller.camera().orbit(12.0f, 33.0f);

    REQUIRE(controller.beginSketch() != document::ObjectId{});
    sketch::Sketch* active = controller.activeSketch();
    REQUIRE(active != nullptr);

    // A frame that is not any global plane and is not axis-aligned: origin off the world origin,
    // u rotated 45 degrees in XY, v tilted out of it. This is the case a face-placed sketch
    // produces, and the one where mapping through a global plane instead gives a wrong answer
    // rather than a coincidentally right one.
    const double k = 1.0 / std::sqrt(2.0);
    sketch::SketchFrame frame;
    frame.origin[0] = 10.0;  frame.origin[1] = -4.0;  frame.origin[2] = 7.0;
    frame.u[0] = k;          frame.u[1] = k;          frame.u[2] = 0.0;
    frame.v[0] = -k;         frame.v[1] = k;          frame.v[2] = 0.0;
    active->setResolvedFrame(frame);

    const render::Viewport vp{1024, 768, 1.0f};
    const render::Camera cam = controller.camera().matrices(vp);

    const double samples[][2]{{0.0, 0.0}, {14.0, 0.0}, {0.0, 9.0}, {-6.0, -11.0}};
    for (const auto& s : samples) {
        // The world point that sketch coordinate corresponds to, computed HERE from the frame —
        // so the test knows the answer independently of the code that will be asked for it.
        const std::array<double, 3> world{
            frame.origin[0] + frame.u[0] * s[0] + frame.v[0] * s[1],
            frame.origin[1] + frame.u[1] * s[0] + frame.v[1] * s[1],
            frame.origin[2] + frame.u[2] * s[0] + frame.v[2] * s[1]};

        const auto pixel = pixelOf(cam, vp, world);
        const auto back = controller.sketchPointAt(pixel[0], pixel[1]);
        REQUIRE(back);
        CHECK_THAT((*back)[0], Catch::Matchers::WithinAbs(s[0], 0.05));
        CHECK_THAT((*back)[1], Catch::Matchers::WithinAbs(s[1], 0.05));
    }
}

TEST_CASE("an edge-on plane refuses rather than inventing a coordinate", "[sketch][viewport]") {
    app::Controller controller;
    controller.setViewportSize(800, 600);

    REQUIRE(controller.beginSketch() != document::ObjectId{});
    REQUIRE(controller.activeSketch() != nullptr);

    const float origin[3]{0.0f, 0.0f, 0.0f};
    const float up[3]{0.0f, 1.0f, 0.0f};

    // Face-on FIRST, and it must answer. Without this half the test could pass because alignTo
    // broke something unrelated, or because sketchPointAt refuses under any aligned camera —
    // a refusal that is always given is not the refusal this is checking for.
    const float faceOn[3]{0.0f, 0.0f, 1.0f};    // the XY sketch's own normal
    controller.camera().alignTo(origin, faceOn, up);
    CHECK(controller.sketchPointAt(400.0f, 300.0f).has_value());

    // Now edge-on: looking ALONG the plane rather than at it. Every ray is parallel to it, so there
    // is no point being pointed at — and a huge finite coordinate would put a line the user cannot
    // see into their sketch, which is worse than nothing happening.
    const float edgeOn[3]{1.0f, 0.0f, 0.0f};
    controller.camera().alignTo(origin, edgeOn, up);
    CHECK_FALSE(controller.sketchPointAt(400.0f, 300.0f).has_value());
}

TEST_CASE("outside the sketch environment there is no point to give", "[sketch][viewport]") {
    app::Controller controller;
    controller.setViewportSize(800, 600);

    // Not editing anything. The shell asks this on every mouse move, so the answer has to be a
    // refusal rather than a crash or a coordinate on some assumed plane.
    CHECK_FALSE(controller.sketchPointAt(400.0f, 300.0f).has_value());
}
