// Pointing the camera at a sketch plane.
//
// Step 1d.2. In-place sketching needs the view to sit face-on to the plane being drawn on, and the
// turntable camera cannot express that: the ordinary case -- a sketch on XY seen from above -- is
// pitch = 90 degrees exactly, the pole the orbit clamp deliberately avoids because `up` degenerates
// there, and a turntable has no roll at all so it cannot put the sketch's u axis along the screen's
// x. A view that is face-on but rotated is worse than useless: every dimension the user reads is
// tilted.
//
// Pure maths, so all of it is provable without a screen. What is NOT claimed here is that anything
// is drawn -- that is step 1d.3, and it is the one part that needs eyes.

#include "cad/app/Controller.h"
#include "cad/render/Camera.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <string>
#include <vector>

using cad::render::CameraController;
using cad::render::Viewport;
using Catch::Approx;

namespace {

Viewport aViewport() {
    Viewport vp;
    vp.width = 800;
    vp.height = 600;
    return vp;
}

/// Where a world point lands in view space, per the convention `bx::mtxLookAt` produces.
///
/// Row-vector: the matrix's COLUMNS hold the basis and row 3 holds the translation, so the view-space
/// x of a point is dot(p, column0) + m[12]. Written out rather than pulled from bx because a test
/// that used the same helper as the code under test would agree with it by construction.
///
/// Right-handed, so the camera looks down -z: a point IN FRONT has negative view-space z, and the
/// third column of the matrix is backward rather than forward.
std::array<float, 3> toViewSpace(const float m[16], const float p[3]) {
    return {p[0] * m[0] + p[1] * m[4] + p[2] * m[8] + m[12],
            p[0] * m[1] + p[1] * m[5] + p[2] * m[9] + m[13],
            p[0] * m[2] + p[1] * m[6] + p[2] * m[10] + m[14]};
}

/// A direction in view space. No translation: a direction is not a point, and adding row 3 to one is
/// the mistake that makes an axis check pass at the origin and nowhere else.
std::array<float, 3> directionToViewSpace(const float m[16], const float d[3]) {
    return {d[0] * m[0] + d[1] * m[4] + d[2] * m[8],
            d[0] * m[1] + d[1] * m[5] + d[2] * m[9],
            d[0] * m[2] + d[1] * m[6] + d[2] * m[10]};
}

float length3(const std::array<float, 3>& v) {
    return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

}  // namespace

TEST_CASE("the view matrix encodes the camera's basis where this test reads it", "[camera]") {
    // Establishes the convention every later assertion depends on, against something known
    // independently: `Camera::eye` is reported separately, so forward must be the normalised
    // direction from the eye to the target. If this fails, the matrix layout assumed by
    // `toViewSpace` is wrong and nothing below it means anything -- which is why it is checked here
    // rather than assumed silently.
    CameraController camera;
    const auto m = camera.matrices(aViewport());
    const auto basis = camera.basis();

    const float target[3]{0, 0, 0};
    float expected[3]{target[0] - m.eye[0], target[1] - m.eye[1], target[2] - m.eye[2]};
    const float len = std::sqrt(expected[0] * expected[0] + expected[1] * expected[1] +
                               expected[2] * expected[2]);
    REQUIRE(len > 1e-6f);
    for (float& v : expected) v /= len;

    for (int i = 0; i < 3; ++i) {
        INFO("axis " << i);
        CHECK(basis.forward[i] == Approx(expected[i]).margin(1e-5));
    }

    // Column 2 holds BACKWARD, because the view space is right-handed.
    CHECK(m.view.m[2] == Approx(-basis.forward[0]).margin(1e-5));
    CHECK(m.view.m[6] == Approx(-basis.forward[1]).margin(1e-5));
    CHECK(m.view.m[10] == Approx(-basis.forward[2]).margin(1e-5));

    // The target sits on the view axis at the camera's distance, in front of it.
    const auto seen = toViewSpace(m.view.m, target);
    CHECK(seen[0] == Approx(0.0).margin(1e-3));
    CHECK(seen[1] == Approx(0.0).margin(1e-3));
    CHECK(seen[2] == Approx(-camera.distance()).epsilon(0.001));
}

TEST_CASE("a front view puts +X on the right of the screen", "[camera]") {
    // The drawing convention every engineer reads a view against, and the assertion that catches the
    // bug this file found: bx defaults to left-handed matrices, and a right-handed Z-up world seen
    // through those comes out MIRRORED. Nothing on screen could show it -- backface culling is
    // deliberately off, so a flipped winding blanks nothing, and every part modelled so far is
    // symmetric. Asking the camera to align to a sketch frame is what surfaced it, because a frame
    // has a handedness.
    //
    // Written against the standard front view rather than a sketch plane on purpose: this is a claim
    // about the RENDERER, and it should keep failing loudly if anyone ever restores bx's default.
    CameraController camera;
    const float origin[3]{0, 0, 0};
    const float towardsPlusY[3]{0, -1, 0};   // the face of the XZ plane that faces -Y
    const float up[3]{0, 0, 1};
    camera.alignTo(origin, towardsPlusY, up);

    const auto m = camera.matrices(aViewport());
    const float plusX[3]{1, 0, 0};
    const float plusZ[3]{0, 0, 1};

    CHECK(directionToViewSpace(m.view.m, plusX)[0] > 0.9f);   // +X to the right, not the left
    CHECK(directionToViewSpace(m.view.m, plusZ)[1] > 0.9f);   // +Z upward
}

TEST_CASE("aligning to a plane puts its axes on the screen's axes", "[camera][sketch]") {
    // The property that makes in-place sketching legible: a rectangle drawn from (0,0) to (10,5) in
    // sketch coordinates has to appear 10 wide and 5 tall, not rotated by some amount that depends
    // on where the camera happened to be. Checked on a plane deliberately not axis-aligned, because
    // an axis-aligned one can pass by coincidence -- its u axis is a world axis, which a turntable
    // could have produced by accident.
    CameraController camera;

    const float origin[3]{12.0f, -3.0f, 7.5f};
    const float u[3]{0.6f, 0.8f, 0.0f};
    const float v[3]{-0.48f, 0.36f, 0.8f};   // perpendicular to u, unit
    const float normal[3]{u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2],
                          u[0] * v[1] - u[1] * v[0]};

    camera.alignTo(origin, normal, v);
    REQUIRE(camera.aligned());

    const auto m = camera.matrices(aViewport());

    // The plane's origin is dead centre.
    const auto centre = toViewSpace(m.view.m, origin);
    CHECK(centre[0] == Approx(0.0).margin(1e-3));
    CHECK(centre[1] == Approx(0.0).margin(1e-3));

    // u runs along screen x, v along screen y, and neither has any depth component -- that last
    // part is what "face-on" actually means, and it is the assertion a merely-centred view fails.
    const auto uSeen = directionToViewSpace(m.view.m, u);
    CHECK(uSeen[0] == Approx(1.0).margin(1e-4));
    CHECK(uSeen[1] == Approx(0.0).margin(1e-4));
    CHECK(uSeen[2] == Approx(0.0).margin(1e-4));

    const auto vSeen = directionToViewSpace(m.view.m, v);
    CHECK(vSeen[0] == Approx(0.0).margin(1e-4));
    CHECK(vSeen[1] == Approx(1.0).margin(1e-4));
    CHECK(vSeen[2] == Approx(0.0).margin(1e-4));

    // Every point of the plane is at one depth. A sketch drawn on it cannot be partly behind the
    // near plane or z-fight against itself.
    const float far1[3]{origin[0] + 40.0f * u[0], origin[1] + 40.0f * u[1], origin[2] + 40.0f * u[2]};
    const float far2[3]{origin[0] - 25.0f * v[0], origin[1] - 25.0f * v[1], origin[2] - 25.0f * v[2]};
    CHECK(toViewSpace(m.view.m, far1)[2] == Approx(centre[2]).margin(1e-2));
    CHECK(toViewSpace(m.view.m, far2)[2] == Approx(centre[2]).margin(1e-2));
}

TEST_CASE("a sketch on XY is looked at from above, not from the pole's edge", "[camera][sketch]") {
    // The case a turntable cannot reach at all: pitch would have to be exactly 90 degrees, and
    // orbit() clamps to 89 precisely because `up` degenerates there. Before alignment existed, the
    // best a shell could do was a view tilted by a degree with the sketch's x axis pointing wherever
    // yaw happened to be left.
    CameraController camera;
    const float origin[3]{0, 0, 0};
    const float normal[3]{0, 0, 1};
    const float up[3]{0, 1, 0};
    camera.alignTo(origin, normal, up);

    const auto basis = camera.basis();
    CHECK(basis.forward[0] == Approx(0.0).margin(1e-6));
    CHECK(basis.forward[1] == Approx(0.0).margin(1e-6));
    CHECK(basis.forward[2] == Approx(-1.0).margin(1e-6));   // straight down
    CHECK(basis.up[1] == Approx(1.0).margin(1e-6));         // sketch +v is screen up
    CHECK(basis.right[0] == Approx(1.0).margin(1e-6));      // sketch +u is screen right

    const auto m = camera.matrices(aViewport());
    CHECK(m.eye[2] == Approx(camera.distance()).epsilon(0.001));
}

TEST_CASE("an up vector along the view direction still gives a usable view", "[camera]") {
    // Degenerate input, and the reason it is handled rather than rejected: a caller can pass a
    // frame's v axis directly, and a zero up vector makes the view matrix singular -- which blanks
    // the viewport with no error at all, the same silent class of failure as the NDC depth mistake
    // recorded in Camera.cpp.
    CameraController camera;
    const float origin[3]{0, 0, 0};
    const float normal[3]{0, 0, 1};
    const float parallel[3]{0, 0, 5};   // along the normal: says nothing about screen orientation

    camera.alignTo(origin, normal, parallel);
    const auto basis = camera.basis();

    CHECK(length3({basis.up[0], basis.up[1], basis.up[2]}) == Approx(1.0).margin(1e-5));
    CHECK(length3({basis.right[0], basis.right[1], basis.right[2]}) == Approx(1.0).margin(1e-5));
    // Still perpendicular to the view, or "up" is not up.
    const float dotFwd = basis.up[0] * basis.forward[0] + basis.up[1] * basis.forward[1] +
                         basis.up[2] * basis.forward[2];
    CHECK(dotFwd == Approx(0.0).margin(1e-5));
}

TEST_CASE("orbiting from an aligned view starts where the view already is", "[camera][sketch]") {
    // Not cosmetic. If orbit restored yaw and pitch from before the sketch was opened, the first
    // pixel of drag would teleport the model somewhere else -- which reads as a bug rather than as a
    // navigation choice. Roll is lost and the pole clamp moves the pitch by about a degree; both are
    // unavoidable for a turntable, and neither is a jump.
    CameraController camera;
    const float origin[3]{0, 0, 0};
    const float normal[3]{0.0f, -1.0f, 0.0f};   // a sketch on XZ, seen from -Y
    const float up[3]{0, 0, 1};

    camera.alignTo(origin, normal, up);
    const auto before = camera.basis();

    camera.orbit(0.0f, 0.0f);   // a drag of nothing: the release must not move the view
    CHECK_FALSE(camera.aligned());
    const auto after = camera.basis();

    for (int i = 0; i < 3; ++i) {
        INFO("axis " << i);
        CHECK(after.forward[i] == Approx(before.forward[i]).margin(1e-3));
        CHECK(after.up[i] == Approx(before.up[i]).margin(1e-3));
    }
}

TEST_CASE("releasing a plan view keeps the heading the sketch had", "[camera][sketch]") {
    // The pole case again, from the other side. Looking straight down, the direction alone carries
    // no heading -- atan2(0,0) is zero, so a naive release swings every plan view east regardless of
    // which way the sketch was oriented. The heading has to be read out of the up vector.
    for (const float degrees : {0.0f, 90.0f, 180.0f, 270.0f}) {
        const float radians = degrees * 3.14159265358979f / 180.0f;
        const float up[3]{std::cos(radians), std::sin(radians), 0.0f};
        const float origin[3]{0, 0, 0};
        const float normal[3]{0, 0, 1};

        CameraController camera;
        camera.alignTo(origin, normal, up);
        const auto before = camera.basis();
        camera.orbit(0.0f, 0.0f);
        const auto after = camera.basis();

        INFO("sketch up at " << degrees << " degrees");
        // Within the clamp: the pitch cannot stay at 90, so `up` tips by ~1 degree. Its horizontal
        // heading must survive, which is what a user notices.
        CHECK(after.up[0] == Approx(before.up[0]).margin(0.03));
        CHECK(after.up[1] == Approx(before.up[1]).margin(0.03));
        CHECK(after.forward[2] < -0.99f);   // still looking essentially downward
    }
}

TEST_CASE("panning an aligned view moves along the screen, not the world", "[camera][sketch]") {
    // Panning recomputed from yaw/pitch would drag a face-on sketch along the world's axes while the
    // pointer moves along the screen's. The two directions disagreeing is felt immediately and
    // diagnosed slowly.
    CameraController camera;
    const float origin[3]{0, 0, 0};
    const float normal[3]{0, 0, 1};
    const float up[3]{0, 1, 0};
    camera.alignTo(origin, normal, up);

    const auto vp = aViewport();
    const auto before = toViewSpace(camera.matrices(vp).view.m, origin);
    camera.pan(30.0f, 0.0f, vp);
    const auto after = toViewSpace(camera.matrices(vp).view.m, origin);

    // The point moved horizontally on screen and nowhere else. Vertical movement or a depth change
    // would both mean the pan used the wrong basis.
    CHECK(std::abs(after[0] - before[0]) > 1.0f);
    CHECK(after[1] == Approx(before[1]).margin(1e-3));
    CHECK(after[2] == Approx(before[2]).margin(1e-3));
    CHECK(camera.aligned());   // panning is not a request to stop being face-on
}

TEST_CASE("a picked face can be looked at face-on, end to end", "[camera][sketch][pick]") {
    // The two halves of step 1d joined: pick a face, get its frame, point the camera at it. Still
    // headless -- what is asserted is the view matrix, not a pixel.
    cad::app::Controller app;
    for (const auto& command : app.commands()) {
        if (command.id == "feature.box") { command.invoke(); break; }
    }
    app.refresh();
    REQUIRE(app.selection().size() == 1);
    const auto id = app.selection().front();

    bool alignedSomething = false;
    for (std::uint32_t slot = 0; slot < 64 && !alignedSomething; ++slot) {
        app.scriptNextPick(slot);
        const auto pick = app.pickAt(10, 10);
        if (!pick.hit || pick.object != id) continue;

        app.scriptNextPick(slot);
        const auto face = app.pickSketchFace(10, 10);
        if (!face) continue;

        app.alignViewTo(face.value().frame);
        REQUIRE(app.camera().aligned());
        alignedSomething = true;

        const auto& frame = face.value().frame;
        const auto m = app.camera().matrices(aViewport());

        const float fu[3]{static_cast<float>(frame.u[0]), static_cast<float>(frame.u[1]),
                          static_cast<float>(frame.u[2])};
        const auto uSeen = directionToViewSpace(m.view.m, fu);
        CHECK(uSeen[0] == Approx(1.0).margin(1e-3));
        CHECK(uSeen[2] == Approx(0.0).margin(1e-3));

        // The face's own origin is centred, so the sketch's (0,0) is where the user is looking.
        const float fo[3]{static_cast<float>(frame.origin[0]), static_cast<float>(frame.origin[1]),
                          static_cast<float>(frame.origin[2])};
        const auto centre = toViewSpace(m.view.m, fo);
        CHECK(centre[0] == Approx(0.0).margin(1e-2));
        CHECK(centre[1] == Approx(0.0).margin(1e-2));
        CHECK(centre[2] < 0.0f);   // in front of the camera, not behind it
    }
    CHECK(alignedSomething);
}
