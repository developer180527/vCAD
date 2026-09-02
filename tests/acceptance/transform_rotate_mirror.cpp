// Rotate and mirror, the two rigid transforms the kernel did not have.
//
// `Transform.h` held translate, extrude and revolve and nothing else, which is why Pattern and
// Mirror could not be built as features however well the naming layer handled copies: there was no
// operation to copy WITH.
//
// Two things are worth asserting about a rigid transform, and they pull in opposite directions.
// It must MOVE the shape — a transform that silently returns its input passes every volume check
// ever written. And it must not DEFORM it — same volume, same shape, only somewhere else. So each
// test below pins a position as well as a size.
//
// Mirror gets more attention than rotate, for one reason: it is the only transform here that
// reverses orientation, and a reflection that reverses the faces without reversing the solid
// produces a body that encloses everything except itself. That body validates. It draws. It is
// wrong only when something subtracts from it.

#include "Model.h"

#include "cad/kernel/Primitives.h"
#include "cad/kernel/Transform.h"
#include "cad/naming/ElementMap.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <numbers>

using cad::kernel::Shape;
using Catch::Approx;

namespace {

constexpr double kOrigin[3] = {0.0, 0.0, 0.0};
constexpr double kZ[3] = {0.0, 0.0, 1.0};
constexpr double kX[3] = {1.0, 0.0, 0.0};

/// A box whose three dimensions differ and which sits ENTIRELY in positive x, y and z.
///
/// Both properties are load-bearing. Unequal sides mean a rotation that swapped two axes cannot
/// hide behind a symmetric result, and an off-origin position means a transform that quietly
/// returned its input has a different centroid from one that did the work.
cad::kernel::Shape aBrick() {
    auto built = cadtest::box(40.0, 20.0, 10.0, 1);
    REQUIRE(built.ok());
    return built.value().shape;
}

}  // namespace

TEST_CASE("a rotation moves the body and keeps its size", "[transform][rotate]") {
    const Shape brick = aBrick();
    const auto before = brick.measure();
    const double volume = brick.volume();
    REQUIRE(volume > 0.0);

    // A quarter turn about Z, through the world origin. The box spans x 0..40 and y 0..20, so its
    // centre of mass is at (20, 10); a quarter turn sends (x, y) to (-y, x).
    const auto turned = cad::kernel::rotate(brick, kOrigin, kZ, std::numbers::pi / 2.0);
    REQUIRE(turned);
    const Shape result = turned.value().shape();

    CHECK(result.volume() == Approx(volume));
    const auto after = result.measure();
    CHECK(after.cx == Approx(-before.cy).margin(1e-9));
    CHECK(after.cy == Approx(before.cx).margin(1e-9));
    CHECK(after.cz == Approx(before.cz).margin(1e-9));
}

TEST_CASE("four quarter turns come back to where they started", "[transform][rotate]") {
    // The cheapest check that a rotation is rigid rather than approximately rigid: error that
    // accumulates has four chances to show, and a transform composed slightly wrong drifts.
    const Shape brick = aBrick();
    const auto before = brick.measure();

    Shape current = brick;
    for (int i = 0; i < 4; ++i) {
        const auto turned = cad::kernel::rotate(current, kOrigin, kZ, std::numbers::pi / 2.0);
        REQUIRE(turned);
        current = turned.value().shape();
    }

    const auto after = current.measure();
    CHECK(after.cx == Approx(before.cx).margin(1e-9));
    CHECK(after.cy == Approx(before.cy).margin(1e-9));
    CHECK(after.cz == Approx(before.cz).margin(1e-9));
    CHECK(current.volume() == Approx(brick.volume()));
}

TEST_CASE("a rotation of nothing is allowed and changes nothing", "[transform][rotate]") {
    // Deliberately NOT refused, unlike a zero-distance extrude. A zero extrude builds a degenerate
    // solid that reports success and then fails everything downstream; a zero rotation builds the
    // input, which is a perfectly good shape. Whether a no-op deserves a row in the feature tree is
    // a question for the layer that has a user to answer to.
    const Shape brick = aBrick();
    const auto turned = cad::kernel::rotate(brick, kOrigin, kZ, 0.0);
    REQUIRE(turned);
    CHECK(turned.value().shape().volume() == Approx(brick.volume()));
    CHECK(turned.value().shape().measure().cx == Approx(brick.measure().cx));
}

TEST_CASE("a rotation about no direction is refused with a reason", "[transform][rotate]") {
    // A zero axis makes gp_Dir throw from inside OCCT. Caught here instead, because the guard would
    // report a construction error naming neither the argument nor what was wrong with it.
    const Shape brick = aBrick();
    constexpr double nowhere[3] = {0.0, 0.0, 0.0};

    const auto refused = cad::kernel::rotate(brick, kOrigin, nowhere, 1.0);
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == cad::kernel::ErrorCode::InvalidInput);
    CHECK_FALSE(refused.error().message.empty());

    // And an angle that is not a number, which would otherwise produce a shape at no coordinates.
    const auto nan = cad::kernel::rotate(brick, kOrigin, kZ,
                                         std::numeric_limits<double>::quiet_NaN());
    REQUIRE_FALSE(nan);
    CHECK(nan.error().code == cad::kernel::ErrorCode::InvalidInput);
}

TEST_CASE("a mirror reflects the body and keeps its size", "[transform][mirror]") {
    const Shape brick = aBrick();
    const auto before = brick.measure();
    const double volume = brick.volume();

    // Reflection in the plane x = 0, whose normal is X.
    const auto flipped = cad::kernel::mirror(brick, kOrigin, kX);
    REQUIRE(flipped);
    const Shape result = flipped.value().shape();

    CHECK(result.volume() == Approx(volume));
    const auto after = result.measure();
    CHECK(after.cx == Approx(-before.cx).margin(1e-9));
    CHECK(after.cy == Approx(before.cy).margin(1e-9));   // unmoved in the plane
    CHECK(after.cz == Approx(before.cz).margin(1e-9));
}

TEST_CASE("a mirror is a reflection, not a half-turn", "[transform][mirror]") {
    // The bug this test exists for, and it is a bug you write by following the API. `gp_Trsf` has
    // TWO SetMirror overloads: the gp_Ax1 one is a half-turn about a LINE — a rotation, which
    // preserves handedness — and the gp_Ax2 one reflects in a PLANE. Both compile, both move the
    // shape, both preserve volume, and on a symmetric part they agree.
    //
    // On this brick they do not. Reflecting in x = 0 sends (x, y, z) to (-x, y, z); a half-turn
    // about the X axis sends it to (x, -y, -z). So y is what tells them apart.
    const Shape brick = aBrick();
    const auto before = brick.measure();
    REQUIRE(before.cy > 1.0);   // off-axis, or the two are indistinguishable here

    const auto flipped = cad::kernel::mirror(brick, kOrigin, kX);
    REQUIRE(flipped);
    const auto after = flipped.value().shape().measure();

    CHECK(after.cy == Approx(before.cy).margin(1e-9));    // a reflection leaves y alone
    CHECK(after.cy != Approx(-before.cy).margin(1e-9));   // a half-turn would have negated it
}

TEST_CASE("a mirrored solid is not inside out", "[transform][mirror]") {
    // The failure the operation checks for before returning. A reflection reverses handedness, and
    // a solid whose faces are not reversed with it encloses its own complement. It passes
    // BRepCheck_Analyzer. Its volume is what gives it away.
    const Shape brick = aBrick();
    const auto flipped = cad::kernel::mirror(brick, kOrigin, kX);
    REQUIRE(flipped);
    const Shape result = flipped.value().shape();

    CHECK(result.volume() > 0.0);
    CHECK(result.validate());
}

TEST_CASE("mirroring twice returns the original", "[transform][mirror]") {
    // A reflection is its own inverse, which is the property that makes it composable — and the
    // one that fails first if the transform is not exactly the plane it claims to be.
    const Shape brick = aBrick();
    const auto before = brick.measure();

    const auto once = cad::kernel::mirror(brick, kOrigin, kX);
    REQUIRE(once);
    const auto twice = cad::kernel::mirror(once.value().shape(), kOrigin, kX);
    REQUIRE(twice);

    const auto after = twice.value().shape().measure();
    CHECK(after.cx == Approx(before.cx).margin(1e-9));
    CHECK(after.cy == Approx(before.cy).margin(1e-9));
    CHECK(after.cz == Approx(before.cz).margin(1e-9));
    CHECK(twice.value().shape().volume() == Approx(brick.volume()));
}

TEST_CASE("a mirror needs a plane, not a point", "[transform][mirror]") {
    const Shape brick = aBrick();
    constexpr double nowhere[3] = {0.0, 0.0, 0.0};
    const auto refused = cad::kernel::mirror(brick, kOrigin, nowhere);
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == cad::kernel::ErrorCode::InvalidInput);
    CHECK_FALSE(refused.error().message.empty());
}

TEST_CASE("names survive a rotation and a mirror", "[transform][naming]") {
    // The reason these two operations belong to the naming layer's lane and not only the kernel's.
    // Both return an `Operation` rather than a bare `Shape` precisely so that
    // `BRepBuilderAPI_Transform::Modified()` can be walked — without that, a rotated body's faces
    // are anonymous, and a fillet placed on one before the rotation has nothing to reattach to.
    //
    // Asserted as: every face that had a name still has one, and no name means two faces.
    auto built = cadtest::box(40.0, 20.0, 10.0, 1);
    REQUIRE(built.ok());
    const auto& source = built.value();
    const std::size_t facesBefore = source.map.size();
    REQUIRE(facesBefore > 0);

    SECTION("rotation") {
        const auto turned = cad::kernel::rotate(source.shape, kOrigin, kZ, std::numbers::pi / 3.0);
        REQUIRE(turned);

        cad::naming::NamingContext naming(2, 0);
        const Shape* input = &source.shape;
        const cad::naming::ElementMap* names = &source.map;
        const auto moved = naming.propagate(turned.value(), {input}, {names});
        if (!moved.ok()) FAIL(moved.error().message << " | " << moved.error().detail);

        CHECK(moved.value().size() == facesBefore);
        CHECK(moved.value().unnamed(turned.value().shape()).empty());
        CHECK(moved.value().collisions().empty());
    }

    SECTION("mirror") {
        const auto flipped = cad::kernel::mirror(source.shape, kOrigin, kX);
        REQUIRE(flipped);

        cad::naming::NamingContext naming(2, 0);
        const Shape* input = &source.shape;
        const cad::naming::ElementMap* names = &source.map;
        const auto moved = naming.propagate(flipped.value(), {input}, {names});
        if (!moved.ok()) FAIL(moved.error().message << " | " << moved.error().detail);

        CHECK(moved.value().size() == facesBefore);
        CHECK(moved.value().unnamed(flipped.value().shape()).empty());
        CHECK(moved.value().collisions().empty());
    }
}
