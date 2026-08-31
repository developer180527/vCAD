/// Offsetting sketch curves.
///
/// The tool is deliberately the SIMPLE one: each selected curve is offset on its own — a line to a
/// parallel line, a circle to a concentric circle. A full CAD offset takes a connected chain and
/// works out the corners, extending or trimming them so the result is still one closed profile.
///
/// These tests pin the behaviour that exists so the difference stays visible: an offset that
/// silently comes apart at the corners is worse than one that never claimed to handle them.

#include "cad/app/Controller.h"
#include "cad/sketch/Sketch.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace cad;
using Catch::Approx;

TEST_CASE("a line offsets to a parallel line at the right distance", "[sketch][offset]") {
    app::Controller c;
    REQUIRE(c.beginSketchOn(sketch::Plane::XY) != document::ObjectId{});
    auto* sketch = c.activeSketch();
    REQUIRE(sketch != nullptr);
    const auto line = sketch->addLine(0, 0, 100, 0);

    c.selectSketchGeometry(line, false);
    REQUIRE(c.offsetSketchSelection(10.0));

    REQUIRE(sketch->geometry().size() == 2);
    const auto& copy = sketch->geometry()[1];
    // Parallel: same direction.
    CHECK(copy.p[1] == Approx(copy.p[3]));
    // And ten millimetres away, on one consistent side.
    CHECK(std::abs(copy.p[1]) == Approx(10.0));
    CHECK(copy.p[0] == Approx(0.0));
    CHECK(copy.p[2] == Approx(100.0));
}

TEST_CASE("the sign chooses the side", "[sketch][offset]") {
    // Two identical calls with opposite signs must land on opposite sides. Without a fixed
    // convention the same call could put the copy anywhere, which makes the tool unusable for the
    // thing it is for: making a wall of a known thickness.
    app::Controller c;
    REQUIRE(c.beginSketchOn(sketch::Plane::XY) != document::ObjectId{});
    auto* sketch = c.activeSketch();
    REQUIRE(sketch != nullptr);
    const auto line = sketch->addLine(0, 0, 100, 0);

    c.selectSketchGeometry(line, false);
    REQUIRE(c.offsetSketchSelection(10.0));
    c.clearSketchSelection();
    c.selectSketchGeometry(line, false);
    REQUIRE(c.offsetSketchSelection(-10.0));

    REQUIRE(sketch->geometry().size() == 3);
    CHECK(sketch->geometry()[1].p[1] == Approx(-sketch->geometry()[2].p[1]));
}

TEST_CASE("a circle offsets concentrically", "[sketch][offset]") {
    app::Controller c;
    REQUIRE(c.beginSketchOn(sketch::Plane::XY) != document::ObjectId{});
    auto* sketch = c.activeSketch();
    REQUIRE(sketch != nullptr);
    const auto circle = sketch->addCircle(5, 5, 20);

    c.selectSketchGeometry(circle, false);
    REQUIRE(c.offsetSketchSelection(3.0));

    REQUIRE(sketch->geometry().size() == 2);
    const auto& copy = sketch->geometry()[1];
    CHECK(copy.kind == sketch::GeoKind::Circle);
    CHECK(copy.p[0] == Approx(5.0));    // same centre
    CHECK(copy.p[1] == Approx(5.0));
    CHECK(copy.p[2] == Approx(23.0));   // radius grown by the offset
}

TEST_CASE("an offset that would collapse a circle is refused", "[sketch][offset]") {
    // Inward by more than the radius has no answer. A circle of negative radius is not geometry,
    // and a zero one is a point the user cannot see or select.
    app::Controller c;
    REQUIRE(c.beginSketchOn(sketch::Plane::XY) != document::ObjectId{});
    auto* sketch = c.activeSketch();
    REQUIRE(sketch != nullptr);
    c.selectSketchGeometry(sketch->addCircle(0, 0, 10), false);

    CHECK_FALSE(c.offsetSketchSelection(-15.0));
    CHECK(sketch->geometry().size() == 1);   // nothing was added
}

TEST_CASE("offset needs a selection and a distance", "[sketch][offset]") {
    app::Controller c;
    REQUIRE(c.beginSketchOn(sketch::Plane::XY) != document::ObjectId{});
    auto* sketch = c.activeSketch();
    REQUIRE(sketch != nullptr);

    CHECK_FALSE(c.offsetSketchSelection(10.0));   // nothing selected

    c.selectSketchGeometry(sketch->addLine(0, 0, 50, 0), false);
    CHECK_FALSE(c.offsetSketchSelection(0.0));    // no distance is not an offset
    CHECK(sketch->geometry().size() == 1);
}

TEST_CASE("several curves offset in one action", "[sketch][offset]") {
    app::Controller c;
    REQUIRE(c.beginSketchOn(sketch::Plane::XY) != document::ObjectId{});
    auto* sketch = c.activeSketch();
    REQUIRE(sketch != nullptr);
    c.selectSketchGeometry(sketch->addLine(0, 0, 100, 0), false);
    c.selectSketchGeometry(sketch->addLine(0, 50, 100, 50), true);

    REQUIRE(c.offsetSketchSelection(5.0));
    CHECK(sketch->geometry().size() == 4);
}
