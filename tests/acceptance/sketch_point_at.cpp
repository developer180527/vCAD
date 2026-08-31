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
#include "cad/kernel/Shape.h"
#include "cad/sketch/Sketch.h"

#include <string>

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

    const document::ObjectId id = controller.beginSketchOn(cad::sketch::Plane::XY);
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

    REQUIRE(controller.beginSketchOn(cad::sketch::Plane::XY) != document::ObjectId{});
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

    REQUIRE(controller.beginSketchOn(cad::sketch::Plane::XY) != document::ObjectId{});
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

TEST_CASE("editing a face-placed sketch aligns the camera and accepts clicks", "[sketch][viewport]") {
    // The end-to-end shape of in-place sketching, and the case that was impossible before:
    // editSketch used to deserialise a face sketch without resolving where the face is, so the
    // sketch had no 3D interpretation of its own coordinates and every click had to be refused.
    app::Controller controller;
    controller.setViewportSize(1200, 800);

    REQUIRE(controller.beginCommand("feature.box"));
    REQUIRE(controller.commitCommand());
    // Found by TYPE, not by position. A new part now starts with three origin datum planes, so
    // the first tree row is a plane and "the box is row zero" was only ever true by accident.
    document::ObjectId boxId{};
    for (const auto& item : controller.tree()) {
        const auto object = controller.document().find(item.id);
        if (object && object->type() == "Box") { boxId = item.id; break; }
    }
    REQUIRE(boxId != document::ObjectId{});
    REQUIRE(boxId != document::ObjectId{});

    // A face of the box, found the way the shell finds one: by picking it. Any planar face will do;
    // what matters is that the sketch ends up on something that is NOT the default XY plane, so an
    // alignment that ignored the face would be visible.
    const auto box = controller.document().find(boxId);
    REQUIRE(box != nullptr);
    REQUIRE(box->output() != nullptr);

    std::string faceName;
    std::array<double, 3> faceNormal{};
    for (const auto& name : box->output()->map.allNames()) {
        const auto shape = box->output()->map.resolve(name);
        if (!shape) continue;
        const auto plane = kernel::planeOf(*shape);
        if (!plane) continue;
        const auto& f = plane.value();
        const double nx = f.u[1] * f.v[2] - f.u[2] * f.v[1];
        const double ny = f.u[2] * f.v[0] - f.u[0] * f.v[2];
        const double nz = f.u[0] * f.v[1] - f.u[1] * f.v[0];
        if (std::abs(nx) > 0.9) {          // a face looking along X: not the default plane
            faceName = name.toString();
            faceNormal = {nx, ny, nz};
            break;
        }
    }
    REQUIRE_FALSE(faceName.empty());

    const document::ObjectId sketchId = controller.addSketchOnFace(boxId, faceName);
    REQUIRE(sketchId != document::ObjectId{});
    REQUIRE(controller.editSketch(sketchId));

    // The camera went to the face, not to XY. Compared against the face's own normal rather than a
    // hard-coded axis, so this still means something if the box's face naming changes.
    const auto basis = controller.camera().basis();
    const double alignment = std::abs(basis.forward[0] * faceNormal[0] +
                                      basis.forward[1] * faceNormal[1] +
                                      basis.forward[2] * faceNormal[2]);
    CHECK_THAT(alignment, Catch::Matchers::WithinAbs(1.0, 1e-3));

    // And a click in the middle of the view now lands on the sketch instead of being refused —
    // which is the whole point, and was impossible before the frame was resolved on edit.
    CHECK(controller.sketchPointAt(600.0f, 400.0f).has_value());
}

TEST_CASE("clicks in the viewport draw into the sketch", "[sketch][viewport]") {
    app::Controller controller;
    controller.setViewportSize(1000, 800);
    REQUIRE(controller.beginSketchOn(cad::sketch::Plane::XY) != document::ObjectId{});

    // Face-on, so every click lands and the pixels below are unambiguous.
    controller.alignCameraToSketch();
    const std::size_t before = controller.activeSketch()->geometry().size();

    SECTION("a line takes two clicks and appears only on the second") {
        controller.setSketchTool(app::Controller::SketchTool::Line);

        REQUIRE(controller.sketchClickAt(300.0f, 300.0f));
        // Nothing yet: a line with one point is not a line, and adding a degenerate one now would
        // leave it in the sketch if the user changed their mind.
        CHECK(controller.activeSketch()->geometry().size() == before);
        CHECK(controller.sketchPending().has_value());

        REQUIRE(controller.sketchClickAt(700.0f, 500.0f));
        CHECK(controller.activeSketch()->geometry().size() == before + 1);
        // The chain CONTINUES from that endpoint — a CAD line tool draws a connected run, so the
        // second click both ends a segment and starts the next. Escape is what finishes it.
        CHECK(controller.sketchPending().has_value());
        controller.endSketchChain();
        CHECK_FALSE(controller.sketchPending().has_value());
    }

    SECTION("switching tools abandons a half-drawn shape") {
        controller.setSketchTool(app::Controller::SketchTool::Line);
        REQUIRE(controller.sketchClickAt(300.0f, 300.0f));
        REQUIRE(controller.sketchPending().has_value());

        controller.setSketchTool(app::Controller::SketchTool::Circle);
        CHECK_FALSE(controller.sketchPending().has_value());

        // And the next two clicks make a CIRCLE, not a line finished with the stale point.
        REQUIRE(controller.sketchClickAt(400.0f, 400.0f));
        REQUIRE(controller.sketchClickAt(500.0f, 400.0f));
        REQUIRE(controller.activeSketch()->geometry().size() == before + 1);
        CHECK(controller.activeSketch()->geometry().back().kind == sketch::GeoKind::Circle);
    }

    SECTION("the select tool draws nothing") {
        controller.setSketchTool(app::Controller::SketchTool::Select);
        CHECK_FALSE(controller.sketchClickAt(300.0f, 300.0f));
        CHECK(controller.activeSketch()->geometry().size() == before);
    }

    SECTION("a line drawn at known pixels lands at the matching sketch coordinates") {
        // The check that ties input to the mapping: the geometry must arrive where sketchPointAt
        // says those pixels are, not merely exist. A tool that dropped the mapping and used raw
        // pixels would pass every count-based assertion above.
        controller.setSketchTool(app::Controller::SketchTool::Line);
        const auto start = controller.sketchPointAt(320.0f, 280.0f);
        const auto end = controller.sketchPointAt(640.0f, 560.0f);
        REQUIRE(start);
        REQUIRE(end);

        REQUIRE(controller.sketchClickAt(320.0f, 280.0f));
        REQUIRE(controller.sketchClickAt(640.0f, 560.0f));

        const auto& drawn = controller.activeSketch()->geometry().back();
        REQUIRE(drawn.kind == sketch::GeoKind::Line);
        CHECK_THAT(drawn.p[0], Catch::Matchers::WithinAbs((*start)[0], 1e-6));
        CHECK_THAT(drawn.p[1], Catch::Matchers::WithinAbs((*start)[1], 1e-6));
        CHECK_THAT(drawn.p[2], Catch::Matchers::WithinAbs((*end)[0], 1e-6));
        CHECK_THAT(drawn.p[3], Catch::Matchers::WithinAbs((*end)[1], 1e-6));
    }
}

TEST_CASE("the in-progress sketch converts to world lines for drawing", "[sketch][viewport]") {
    app::Controller controller;
    controller.setViewportSize(1000, 800);
    REQUIRE(controller.beginSketchOn(cad::sketch::Plane::XY) != document::ObjectId{});

    sketch::Sketch* active = controller.activeSketch();
    REQUIRE(active != nullptr);

    // A tilted frame again: on XY the sketch's (u, v) and the world's (x, y) coincide, so a
    // conversion that ignored the frame entirely would produce the right numbers by accident.
    const double k = 1.0 / std::sqrt(2.0);
    sketch::SketchFrame frame;
    frame.origin[0] = 5.0;  frame.origin[1] = 2.0;  frame.origin[2] = -3.0;
    frame.u[0] = k;         frame.u[1] = 0.0;       frame.u[2] = k;
    frame.v[0] = 0.0;       frame.v[1] = 1.0;       frame.v[2] = 0.0;
    active->setResolvedFrame(frame);

    const std::size_t existing = controller.sketchOverlayVertices().size();
    const std::uint64_t before = controller.sketchOverlayRevision();

    active->addLine(2.0, 3.0, 8.0, 11.0);

    const auto lines = controller.sketchOverlayVertices();
    // One segment: two endpoints, three floats each.
    REQUIRE(lines.size() == existing + 6);

    // Where the sketch itself says those coordinates are. Asserted against to3d rather than against
    // numbers written here, because to3d is what the SOLID will be built from — if the overlay and
    // the profile disagree, the user draws one shape and gets another.
    const auto start = active->to3d(2.0, 3.0);
    const auto end = active->to3d(8.0, 11.0);
    for (int i = 0; i < 3; ++i) {
        CHECK_THAT(static_cast<double>(lines[existing + i]),
                   Catch::Matchers::WithinAbs(start[i], 1e-4));
        CHECK_THAT(static_cast<double>(lines[existing + 3 + i]),
                   Catch::Matchers::WithinAbs(end[i], 1e-4));
    }

    // The revision tracks the geometry.
    const std::uint64_t after = controller.sketchOverlayRevision();
    CHECK(after != before);

    // …and NOT the camera. This is the whole point of a digest over a counter: an orbit must not
    // re-upload the sketch, and nothing but this check can tell the difference.
    controller.camera().orbit(30.0f, 15.0f);
    CHECK(controller.sketchOverlayRevision() == after);
}

TEST_CASE("a half-drawn shape follows the pointer", "[sketch][viewport]") {
    app::Controller controller;
    controller.setViewportSize(1000, 800);
    REQUIRE(controller.beginSketchOn(cad::sketch::Plane::XY) != document::ObjectId{});
    controller.alignCameraToSketch();
    controller.setSketchTool(app::Controller::SketchTool::Line);

    const std::size_t committed = controller.activeSketch()->geometry().size();
    const std::size_t settled = controller.sketchOverlayVertices().size();

    SECTION("nothing is previewed before the first click") {
        // The pointer means nothing yet. Following it here would draw a band from the sketch
        // origin to the mouse, which looks like a stray line the user cannot delete.
        CHECK_FALSE(controller.sketchHoverAt(500.0f, 400.0f));
        CHECK(controller.sketchPreviewVertices().empty());
    }

    SECTION("after the first click the band tracks the pointer, and commits nothing") {
        REQUIRE(controller.sketchClickAt(300.0f, 300.0f));
        REQUIRE(controller.sketchHoverAt(700.0f, 500.0f));

        const auto preview = controller.sketchPreviewVertices();
        CHECK_FALSE(preview.empty());

        // The preview is SEPARATE from the committed geometry, in both lists: nothing was added to
        // the sketch, and nothing was added to the overlay the committed geometry is drawn from.
        // A preview that quietly committed would leave a stray line whenever the user changed
        // their mind, which is the failure this separation exists to make impossible.
        CHECK(controller.activeSketch()->geometry().size() == committed);
        CHECK(controller.sketchOverlayVertices().size() == settled);

        // It STARTS at the first click. Exact, because that endpoint is not affected by dashing.
        const auto from = controller.activeSketch()->to3d((*controller.sketchPending())[0],
                                                          (*controller.sketchPending())[1]);
        for (int i = 0; i < 3; ++i) {
            CHECK_THAT(static_cast<double>(preview[i]), Catch::Matchers::WithinAbs(from[i], 1e-4));
        }

        // And every dash lies ON the line to the pointer. Asserted as collinearity rather than as
        // an endpoint, because the number of dashes depends on zoom and the last one stops short
        // of the end by design — but a band that pointed somewhere else would still be caught.
        const auto at = controller.sketchPointAt(700.0f, 500.0f);
        REQUIRE(at);
        const auto to = controller.activeSketch()->to3d((*at)[0], (*at)[1]);
        const double dx = to[0] - from[0];
        const double dy = to[1] - from[1];
        const double dz = to[2] - from[2];
        const double length = std::sqrt(dx * dx + dy * dy + dz * dz);
        REQUIRE(length > 1e-6);
        double farthest = 0.0;
        for (std::size_t v = 0; v + 2 < preview.size(); v += 3) {
            const double px = preview[v] - from[0];
            const double py = preview[v + 1] - from[1];
            const double pz = preview[v + 2] - from[2];
            const double along = (px * dx + py * dy + pz * dz) / length;
            // Perpendicular distance from the line: zero for every point on it.
            const double cx = px - along * dx / length;
            const double cy = py - along * dy / length;
            const double cz = pz - along * dz / length;
            CHECK(std::sqrt(cx * cx + cy * cy + cz * cz) < 1e-3);
            farthest = std::max(farthest, along);
        }
        // It reaches the pointer rather than petering out a third of the way: the band is a promise
        // about where the second click lands, and one that stops short misleads.
        CHECK(farthest > length * 0.9);

        // A repeat of the same position is not a repaint.
        CHECK_FALSE(controller.sketchHoverAt(700.0f, 500.0f));
    }

    SECTION("finishing the shape replaces the band with real geometry") {
        REQUIRE(controller.sketchClickAt(300.0f, 300.0f));
        REQUIRE(controller.sketchHoverAt(700.0f, 500.0f));
        REQUIRE(controller.sketchClickAt(700.0f, 500.0f));

        CHECK(controller.activeSketch()->geometry().size() == committed + 1);
        CHECK(controller.sketchOverlayVertices().size() == settled + 6);
        // The band is gone but the CHAIN is not: the pointer has not moved since the click, so
        // there is nothing to preview yet, and the next move starts a band from the new endpoint.
        CHECK(controller.sketchPreviewVertices().empty());
        CHECK(controller.sketchPending().has_value());
    }

    SECTION("switching tools drops the band with the pending point") {
        REQUIRE(controller.sketchClickAt(300.0f, 300.0f));
        REQUIRE(controller.sketchHoverAt(700.0f, 500.0f));
        controller.setSketchTool(app::Controller::SketchTool::Select);
        CHECK(controller.sketchPreviewVertices().empty());
        CHECK(controller.sketchOverlayVertices().size() == settled);
    }

    SECTION("the dashes keep their size on screen as the view zooms") {
        REQUIRE(controller.sketchClickAt(300.0f, 300.0f));
        REQUIRE(controller.sketchHoverAt(700.0f, 500.0f));
        const std::size_t atRest = controller.sketchPreviewVertices().size();

        // The invariant, stated without depending on which way zoom's sign runs: dash COUNT moves
        // inversely with world-units-per-pixel. Zoom in and the band covers more pixels, so it
        // takes more dashes; a dash measured in world units would hold the count fixed and turn
        // into a solid line at one end of the range and a row of dots at the other.
        const render::Viewport vp{1000, 800, 1.0f};
        const float before = controller.camera().worldPerPixel(vp);
        controller.camera().zoom(6.0f);
        const float after = controller.camera().worldPerPixel(vp);
        REQUIRE(after != before);

        controller.sketchHoverAt(700.0f, 500.0f);
        const std::size_t zoomed = controller.sketchPreviewVertices().size();
        CHECK((after < before) == (zoomed > atRest));
    }
}

TEST_CASE("orbit mode is a mode the model owns", "[sketch][viewport]") {
    app::Controller controller;
    // Off by default: a plain left drag selects, which is what a viewport is mostly for.
    CHECK_FALSE(controller.orbitMode());
    controller.setOrbitMode(true);
    CHECK(controller.orbitMode());
    controller.setOrbitMode(false);
    CHECK_FALSE(controller.orbitMode());
}
