/// Applying constraints to a selection.
///
/// # The gap this closes
///
/// Twelve constraint kinds solved and the UI could apply five of them: coincident and tangent
/// automatically at a join, horizontal and vertical by inference while drawing, distance by typing.
/// Parallel, perpendicular, equal length and manual horizontal/vertical had no route in at all — so
/// a sketch could be drawn and could not be fully constrained, which is the difference between a
/// drawing and a model.
///
/// # Why the menu is computed rather than fixed
///
/// A wall of buttons that mostly produce error messages teaches the user to ignore the wall.
/// Shapr3D's menu "automatically highlights valid options based on your selected elements", and
/// what is asserted here is that ours cannot offer something the model would then refuse.

#include "cad/app/Controller.h"
#include "cad/sketch/Sketch.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <string>

using namespace cad;
using Catch::Approx;

namespace {

bool offers(const std::vector<sketch::ConstraintKind>& menu, sketch::ConstraintKind kind) {
    return std::find(menu.begin(), menu.end(), kind) != menu.end();
}

std::size_t count(const sketch::Sketch& s, sketch::ConstraintKind kind) {
    const auto& all = s.constraints();
    return static_cast<std::size_t>(
        std::count_if(all.begin(), all.end(), [kind](const auto& c) { return c.kind == kind; }));
}

}   // namespace

TEST_CASE("the menu offers only what the selection can take", "[sketch][constraints]") {
    app::Controller c;
    REQUIRE(c.beginSketchOn(sketch::Plane::XY) != document::ObjectId{});
    auto* sketch = c.activeSketch();
    REQUIRE(sketch != nullptr);
    const auto a = sketch->addLine(0, 0, 100, 5);
    const auto b = sketch->addLine(100, 5, 100, 60);
    const auto circle = sketch->addCircle(200, 200, 20);

    // Nothing selected: nothing to offer.
    CHECK(c.applicableConstraints().empty());

    // One line describes a direction, so the two that fix a direction apply — and the ones that
    // relate two lines do not.
    c.selectSketchGeometry(a, false);
    auto menu = c.applicableConstraints();
    CHECK(offers(menu, sketch::ConstraintKind::Horizontal));
    CHECK(offers(menu, sketch::ConstraintKind::Vertical));
    CHECK_FALSE(offers(menu, sketch::ConstraintKind::Parallel));
    CHECK_FALSE(offers(menu, sketch::ConstraintKind::EqualLength));

    c.selectSketchGeometry(b, true);
    menu = c.applicableConstraints();
    CHECK(offers(menu, sketch::ConstraintKind::Parallel));
    CHECK(offers(menu, sketch::ConstraintKind::Perpendicular));
    CHECK(offers(menu, sketch::ConstraintKind::EqualLength));
    // Two straight lines cannot be tangent: tangency is a shared direction at a point, and two
    // lines that share one are one line.
    CHECK_FALSE(offers(menu, sketch::ConstraintKind::Tangent));

    // A line and a circle can be.
    c.selectSketchGeometry(a, false);
    c.selectSketchGeometry(circle, true);
    CHECK(offers(c.applicableConstraints(), sketch::ConstraintKind::Tangent));
}

TEST_CASE("a constraint the menu did not offer is refused", "[sketch][constraints]") {
    // The menu and the model must agree. If a shell can talk the Controller into applying something
    // the menu would not show, then the menu is decoration.
    app::Controller c;
    REQUIRE(c.beginSketchOn(sketch::Plane::XY) != document::ObjectId{});
    auto* sketch = c.activeSketch();
    REQUIRE(sketch != nullptr);
    c.selectSketchGeometry(sketch->addLine(0, 0, 100, 0), false);

    CHECK_FALSE(c.applySketchConstraint(sketch::ConstraintKind::Parallel));   // needs two lines
    CHECK(count(*sketch, sketch::ConstraintKind::Parallel) == 0);
}

TEST_CASE("perpendicular actually moves the geometry", "[sketch][constraints]") {
    // Recorded is not applied. Two lines drawn at a careless angle must come out at ninety degrees.
    app::Controller c;
    REQUIRE(c.beginSketchOn(sketch::Plane::XY) != document::ObjectId{});
    auto* sketch = c.activeSketch();
    REQUIRE(sketch != nullptr);
    const auto a = sketch->addLine(0, 0, 100, 0);
    const auto b = sketch->addLine(100, 0, 130, 60);   // roughly, but not at all, perpendicular
    sketch->coincident(a, sketch::PointRef::End, b, sketch::PointRef::Start);

    c.selectSketchGeometry(a, false);
    c.selectSketchGeometry(b, true);
    REQUIRE(c.applySketchConstraint(sketch::ConstraintKind::Perpendicular));

    const auto* la = sketch->find(a);
    const auto* lb = sketch->find(b);
    REQUIRE(la != nullptr);
    REQUIRE(lb != nullptr);
    const double ax = la->p[2] - la->p[0], ay = la->p[3] - la->p[1];
    const double bx = lb->p[2] - lb->p[0], by = lb->p[3] - lb->p[1];
    const double cosine = std::abs(ax * bx + ay * by) / (std::hypot(ax, ay) * std::hypot(bx, by));
    INFO("cosine of the angle between them: " << cosine);
    CHECK(cosine < 0.01);   // within about half a degree of square
}

TEST_CASE("a conflicting constraint is reported, not hidden", "[sketch][constraints]") {
    // The project's deliberate choice: KEEP it and say so. The solver names which constraints
    // cannot all hold, so the user removes the one they meant — reverting automatically would hide
    // that the sketch was already close to over-constrained.
    //
    // What this really guards is the DETECTION. planegcs reports success on the first solve of a
    // system that cannot hold and only names the conflict when asked again, so a single solve left
    // the message unsaid and the sketch quietly unsolvable.
    app::Controller c;
    REQUIRE(c.beginSketchOn(sketch::Plane::XY) != document::ObjectId{});
    auto* sketch = c.activeSketch();
    REQUIRE(sketch != nullptr);
    const auto a = sketch->addLine(0, 0, 100, 0);
    const auto b = sketch->addLine(0, 20, 100, 20);
    sketch->horizontal(a);
    sketch->horizontal(b);

    std::string said;
    c.onStatus([&](const std::string& text) { said = text; });

    c.selectSketchGeometry(a, false);
    c.selectSketchGeometry(b, true);
    REQUIRE(c.applySketchConstraint(sketch::ConstraintKind::Perpendicular));

    INFO("status: " << said);
    CHECK(said.find("over-constrained") != std::string::npos);
}

TEST_CASE("horizontal applies to every selected line at once", "[sketch][constraints]") {
    app::Controller c;
    REQUIRE(c.beginSketchOn(sketch::Plane::XY) != document::ObjectId{});
    auto* sketch = c.activeSketch();
    REQUIRE(sketch != nullptr);
    c.selectSketchGeometry(sketch->addLine(0, 0, 100, 3), false);
    c.selectSketchGeometry(sketch->addLine(0, 40, 100, 44), true);

    REQUIRE(c.applySketchConstraint(sketch::ConstraintKind::Horizontal));
    CHECK(count(*sketch, sketch::ConstraintKind::Horizontal) == 2);
    for (const auto& g : sketch->geometry()) {
        CHECK(g.p[1] == Approx(g.p[3]).margin(1e-6));
    }
}
