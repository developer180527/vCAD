/// Asking the model a question.
///
/// # Why this is a system and not a tool
///
/// Modelling is a loop: make a change, check it, adjust. vCAD could do the first and the third and
/// not the second. `Shape::measure` and `Shape::volume` had existed in the kernel since M1 and
/// nothing above the kernel ever called them, so there was no way for a user to learn how long an
/// edge was. A modeller you cannot interrogate is one you have to trust.
///
/// These tests are mostly about the ANSWERS BEING RIGHT, because a measure tool that is merely
/// present is worse than none: a wrong number is acted on, and a missing one is only inconvenient.
/// A 40x30x20 box has known length, area and volume, so the arithmetic is checkable rather than
/// self-consistent.

#include "cad/app/Controller.h"
#include "cad/kernel/Measurement.h"
#include "cad/kernel/Primitives.h"
#include "cad/kernel/Transform.h"

#include <algorithm>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace cad;
using Catch::Approx;

namespace {

document::ObjectId boxIn(app::Controller& c) {
    REQUIRE(c.beginCommand("feature.box"));
    REQUIRE(c.commitCommand());
    return c.selection().front();
}

std::vector<naming::ElementName> elementsOfType(const app::Controller& c, document::ObjectId id,
                                                kernel::ShapeType type) {
    std::vector<naming::ElementName> out;
    const auto object = c.document().find(id);
    if (!object || object->output() == nullptr) return out;
    for (const auto& name : object->output()->map.allNames()) {
        const auto shape = object->output()->map.resolve(name);
        if (shape && shape->type() == type) out.push_back(name);
    }
    return out;
}

std::string valueOf(const std::vector<app::Controller::MeasureRow>& rows, const std::string& label) {
    for (const auto& row : rows) {
        if (row.label == label) return row.value;
    }
    return {};
}

}   // namespace

TEST_CASE("a box measures what a box measures", "[measure]") {
    // Checked against arithmetic, not against a previous run. 100 x 60 x 40 is what feature.box
    // creates: volume 100*60*40 = 240000 mm3, surface 2*(6000 + 4000 + 2400) = 24800 mm2.
    //
    // The first version of this test said 27200 and OCCT disagreed. Worth recording: the number
    // here is worked out independently, so when the two differ one of them is wrong and it is not
    // automatically the code.
    auto made = kernel::makeBox(100.0, 60.0, 40.0);
    REQUIRE(made.ok());
    const auto measured = kernel::measure(made.value().op.shape());
    REQUIRE(measured.ok());

    CHECK(measured.value().volume == Approx(240000.0));
    CHECK(measured.value().area == Approx(24800.0));
    // Centre of a box built from the origin.
    CHECK(measured.value().centre[0] == Approx(50.0));
    CHECK(measured.value().centre[1] == Approx(30.0));
    CHECK(measured.value().centre[2] == Approx(20.0));
}

TEST_CASE("an edge measures its length, a face its area", "[measure]") {
    auto made = kernel::makeBox(100.0, 60.0, 40.0);
    REQUIRE(made.ok());
    const auto shape = made.value().op.shape();

    double longest = 0.0;
    for (const auto& edge : shape.subShapes(kernel::ShapeType::Edge)) {
        const auto m = kernel::measure(edge);
        REQUIRE(m.ok());
        longest = std::max(longest, m.value().length);
        // An edge encloses nothing, so it must not claim a volume.
        CHECK(m.value().volume == Approx(0.0));
    }
    CHECK(longest == Approx(100.0));

    double largest = 0.0;
    for (const auto& face : shape.subShapes(kernel::ShapeType::Face)) {
        const auto m = kernel::measure(face);
        REQUIRE(m.ok());
        largest = std::max(largest, m.value().area);
        // A face bounds nothing either. BRepGProp will happily answer VolumeProperties for one and
        // the number is an artefact of where the origin is, which is why it is not asked.
        CHECK(m.value().volume == Approx(0.0));
    }
    CHECK(largest == Approx(6000.0));   // the 100 x 60 face
}

TEST_CASE("a hole's diameter is answerable", "[measure]") {
    // The single most common measurement in mechanical CAD, and the reason radius is in the
    // kernel struct rather than left to the caller to work out from geometry.
    auto made = kernel::makeCylinder(12.5, 50.0);
    REQUIRE(made.ok());

    bool sawRadius = false;
    for (const auto& face : made.value().shape().subShapes(kernel::ShapeType::Face)) {
        const auto m = kernel::measure(face);
        REQUIRE(m.ok());
        if (!m.value().hasRadius) continue;
        sawRadius = true;
        CHECK(m.value().radius == Approx(12.5));
    }
    CHECK(sawRadius);
}

TEST_CASE("distance between two shapes is the gap, and zero when they touch", "[measure]") {
    auto first = kernel::makeBox(10.0, 10.0, 10.0);
    auto second = kernel::makeBox(10.0, 10.0, 10.0);
    REQUIRE(first.ok());
    REQUIRE(second.ok());

    auto apart = kernel::translate(second.value().op.shape(), 25.0, 0.0, 0.0);
    REQUIRE(apart.ok());
    const auto gap = kernel::distanceBetween(first.value().op.shape(), apart.value().shape());
    REQUIRE(gap.ok());
    CHECK(gap.value() == Approx(15.0));   // 25 apart, 10 wide

    // Touching is ZERO, not a failure. "These two faces are 0 apart" is exactly what a user needs
    // to hear when checking a fit.
    auto touching = kernel::translate(second.value().op.shape(), 10.0, 0.0, 0.0);
    REQUIRE(touching.ok());
    const auto contact = kernel::distanceBetween(first.value().op.shape(),
                                                 touching.value().shape());
    REQUIRE(contact.ok());
    CHECK(contact.value() == Approx(0.0).margin(1e-9));
}

TEST_CASE("measuring nothing fails rather than answering zero", "[measure]") {
    // Zero is a legitimate length, so a failure that returns it is indistinguishable from a real
    // measurement of a degenerate element -- the same trap Shape::volume avoids with NaN.
    const kernel::Shape nothing;
    CHECK_FALSE(kernel::measure(nothing).ok());
    CHECK_FALSE(kernel::distanceBetween(nothing, nothing).ok());
}

TEST_CASE("the readout says nothing when nothing is selected", "[measure][controller]") {
    app::Controller c;
    boxIn(c);
    c.clearSelection();
    CHECK(c.measureSelection().empty());
}

TEST_CASE("selecting an edge reports its length in the user's units", "[measure][controller]") {
    app::Controller c;
    const auto box = boxIn(c);
    const auto edges = elementsOfType(c, box, kernel::ShapeType::Edge);
    REQUIRE_FALSE(edges.empty());

    c.setSelectionLevel(app::Controller::SelectionLevel::Auto);
    c.selectElement(box, edges[0], false);

    const auto rows = c.measureSelection();
    REQUIRE_FALSE(rows.empty());
    CHECK_FALSE(valueOf(rows, "Length").empty());
    CHECK(valueOf(rows, "Length").find("mm") != std::string::npos);

    // And in inches when that is what the user works in. The number changes, the model does not.
    auto preferences = c.preferences();
    preferences.displayUnits = units::UnitSystem::Inch;
    c.setPreferences(preferences);
    const auto inches = c.measureSelection();
    CHECK(valueOf(inches, "Length").find("in") != std::string::npos);
    CHECK(valueOf(inches, "Length") != valueOf(rows, "Length"));
}

TEST_CASE("selecting two elements adds the distance between them", "[measure][controller]") {
    // The question that needs two, and the one a fit check is made of.
    app::Controller c;
    const auto box = boxIn(c);
    const auto faces = elementsOfType(c, box, kernel::ShapeType::Face);
    REQUIRE(faces.size() >= 2);

    c.setSelectionLevel(app::Controller::SelectionLevel::Auto);
    c.selectElement(box, faces[0], false);
    const auto one = c.measureSelection();
    CHECK(valueOf(one, "Distance").empty());

    c.selectElement(box, faces[1], true);
    const auto two = c.measureSelection();
    CHECK_FALSE(valueOf(two, "Distance").empty());

    // Adding to the selection must not take information away: each element is still described.
    CHECK_FALSE(valueOf(two, "First area").empty());
    CHECK_FALSE(valueOf(two, "Second area").empty());
}
