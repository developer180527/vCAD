/// An arc stays where it was drawn when its sketch is solved.
///
/// # The bug
///
/// A solve writes each arc's angles back from its solved endpoints with atan2, which answers in
/// (-pi, pi]. An arc drawn from 90 degrees to 270 degrees -- the left half of a circle -- came back as
/// 90 to -90, and the wire is built through the MID-angle, (start + end) / 2: 180 before the solve,
/// 0 after. So the arc flipped to the right half. Every arc passing through its centre's -X direction
/// did this, in every sketch, constrained or not, because computing a sketch feature solves it.
///
/// Found by SolidCraft's reference part 33 (tests/acceptance/oracle_solidcraft.cpp): a slot whose
/// left end came back bulging inward, 24% short of its volume.

#include "cad/kernel/Measurement.h"
#include "cad/sketch/Sketch.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <numbers>

using namespace cad;

namespace {

constexpr double pi = std::numbers::pi;

/// The x extent of a sketch's curves, as the kernel builds them.
std::pair<double, double> xExtent(const sketch::Sketch& s) {
    const auto wire = s.toWire();
    REQUIRE(wire);
    const auto box = kernel::boundingBox(wire.value());
    REQUIRE(box);
    return {box.value().min[0], box.value().max[0]};
}

}  // namespace

TEST_CASE("an arc through the -X direction keeps its side when solved", "[sketch][arcs]") {
    // The left half of a circle of radius 8 about the origin, closed by its diameter.
    sketch::Sketch s(sketch::Plane::XY);
    s.addArc(0, 0, 8, pi / 2, 3 * pi / 2);
    s.addLine(0, -8, 0, 8);

    const auto before = xExtent(s);
    CHECK(before.first < -7.99);  // drawn on the left
    s.solve();
    const auto after = xExtent(s);
    CHECK(after.first < -7.99);   // and still there
    CHECK(after.second < 1e-6);
}

TEST_CASE("an arc that does not cross -X is unchanged by a solve", "[sketch][arcs]") {
    // The control: the right half never crossed the atan2 cut, and never flipped.
    sketch::Sketch s(sketch::Plane::XY);
    s.addArc(0, 0, 8, -pi / 2, pi / 2);
    s.addLine(0, 8, 0, -8);
    s.solve();
    const auto after = xExtent(s);
    CHECK(after.first > -1e-6);
    CHECK(after.second > 7.99);
}

TEST_CASE("a major arc stays major when solved", "[sketch][arcs]") {
    // 300 degrees, from 30 to 330, through 180: after the old write-back it came out as the 60-degree
    // minor arc from 30 to -30 -- the same endpoints, a different curve.
    sketch::Sketch s(sketch::Plane::XY);
    s.addArc(0, 0, 8, pi / 6, 11 * pi / 6);
    s.addLine(8 * std::cos(11 * pi / 6), 8 * std::sin(11 * pi / 6), 0, 0);
    s.addLine(0, 0, 8 * std::cos(pi / 6), 8 * std::sin(pi / 6));
    s.solve();
    CHECK(xExtent(s).first < -7.99);
}
