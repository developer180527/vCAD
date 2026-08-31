/// Parameters as the application exposes them: the table, and typing a formula into a field.
///
/// The document layer already guarantees the rules -- resolution order, cycles, entry units. What
/// is only true at this level is that an EDIT behaves like an edit: it is refused as a whole or
/// applied as a whole, it is undoable in one step, and a formula typed into an ordinary field is
/// remembered as a formula rather than as the number it produced today.

#include "cad/app/Controller.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace cad;
using Catch::Approx;

namespace {

double lengthOf(const app::Controller& c, document::ObjectId id, const std::string& name) {
    const auto object = c.document().find(id);
    REQUIRE(object);
    const auto* value = object->find(name);
    REQUIRE(value != nullptr);
    return std::get<units::Length>(*value).base();
}

document::ObjectId boxIn(app::Controller& c) {
    REQUIRE(c.beginCommand("feature.box"));
    REQUIRE(c.commitCommand());
    return c.selection().front();
}

}   // namespace

TEST_CASE("a parameter can be added and read back", "[controller][parameters]") {
    app::Controller c;
    REQUIRE(c.setParameter("width", "40mm"));

    const auto rows = c.parameters();
    REQUIRE(rows.size() == 1);
    CHECK(rows.front().name == "width");
    CHECK(rows.front().problem.empty());
    CHECK(rows.front().expression.empty());   // a plain number is not a formula
    CHECK(rows.front().value.find("40") != std::string::npos);
}

TEST_CASE("a derived parameter reports its formula and its value", "[controller][parameters]") {
    app::Controller c;
    REQUIRE(c.setParameter("width", "40mm"));
    REQUIRE(c.setParameter("wall", "width / 8"));

    for (const auto& row : c.parameters()) {
        if (row.name != "wall") continue;
        CHECK(row.expression == "width / 8");
        CHECK(row.value.find("5") != std::string::npos);
    }
}

TEST_CASE("a refused parameter changes nothing at all", "[controller][parameters]") {
    // Whole or not at all. Storing it and reporting the problem afterwards leaves a document whose
    // parameters cannot all be resolved -- a state the user did not ask for and cannot easily leave.
    app::Controller c;
    REQUIRE(c.setParameter("a", "10mm"));
    REQUIRE(c.setParameter("b", "a + 1mm"));

    CHECK_FALSE(c.setParameter("a", "b + 1mm"));   // would close a cycle
    CHECK(c.parameters().size() == 2);
    for (const auto& row : c.parameters()) {
        INFO(row.name << ": " << row.problem);
        CHECK(row.problem.empty());
    }

    CHECK_FALSE(c.setParameter("c", "nosuch * 2"));
    CHECK(c.parameters().size() == 2);
    CHECK_FALSE(c.setParameter("pi", "3"));        // shadows a built-in
    CHECK(c.parameters().size() == 2);
}

TEST_CASE("a formula typed into a field is remembered as a formula",
          "[controller][parameters]") {
    app::Controller c;
    REQUIRE(c.setParameter("width", "40mm"));
    const auto box = boxIn(c);

    REQUIRE(c.setProperty(box, "dx", "width * 2"));
    CHECK(lengthOf(c, box, "dx") == Approx(80.0));

    // Shown back as the formula, not as the 80 -- a field showing 80 invites the user to retype 80.
    bool sawExpression = false;
    for (const auto& row : c.properties(box)) {
        if (row.name == "dx") sawExpression = row.expression == "width * 2";
    }
    CHECK(sawExpression);

    // And it follows the parameter.
    REQUIRE(c.setParameter("width", "60mm"));
    CHECK(lengthOf(c, box, "dx") == Approx(120.0));
}

TEST_CASE("a plain number typed into a field breaks the link", "[controller][parameters]") {
    app::Controller c;
    REQUIRE(c.setParameter("width", "40mm"));
    const auto box = boxIn(c);
    REQUIRE(c.setProperty(box, "dx", "width * 2"));

    REQUIRE(c.setProperty(box, "dx", "15mm"));
    REQUIRE(c.setParameter("width", "60mm"));
    CHECK(lengthOf(c, box, "dx") == Approx(15.0));   // no longer driven, on purpose
}

TEST_CASE("editing a parameter is one undo step", "[controller][parameters]") {
    // Two steps would mean the first press of undo appears to do nothing -- the bug that comes back
    // every time an edit commits twice.
    app::Controller c;
    REQUIRE(c.setParameter("width", "40mm"));
    const auto box = boxIn(c);
    REQUIRE(c.setProperty(box, "dx", "width * 2"));
    REQUIRE(c.setParameter("width", "60mm"));
    REQUIRE(lengthOf(c, box, "dx") == Approx(120.0));

    REQUIRE(c.undo());
    CHECK(lengthOf(c, box, "dx") == Approx(80.0));
}

TEST_CASE("creating a feature with a formula keeps the link", "[controller][parameters]") {
    // The moment a user is most likely to reach for a parameter. Evaluating it and then forgetting
    // it would make the box the right size exactly once.
    app::Controller c;
    REQUIRE(c.setParameter("width", "40mm"));

    REQUIRE(c.beginCommand("feature.box"));
    REQUIRE(c.setCommandParameter("dx", "width * 2"));
    REQUIRE(c.commitCommand());
    const auto box = c.selection().front();
    CHECK(lengthOf(c, box, "dx") == Approx(80.0));

    REQUIRE(c.setParameter("width", "60mm"));
    CHECK(lengthOf(c, box, "dx") == Approx(120.0));

    // And it was ONE undo step, not two.
    REQUIRE(c.undo());
    CHECK(lengthOf(c, box, "dx") == Approx(80.0));
}

TEST_CASE("renaming carries the value exactly and undoes in one step",
          "[controller][parameters]") {
    app::Controller c;
    // A value with more decimals than any field displays. Add-then-remove through the table's own
    // text would round this; a rename must not be able to change a number at all.
    REQUIRE(c.setParameter("width", "40.00048828125mm"));
    REQUIRE(c.setParameter("wall", "width / 8"));

    REQUIRE(c.renameParameter("width", "plate_width"));
    CHECK(c.document().parameter("width") == nullptr);
    const auto* renamed = c.document().parameter("plate_width");
    REQUIRE(renamed != nullptr);
    CHECK(std::get<units::Length>(renamed->value).base() == 40.00048828125);

    // `wall` still says `width / 8`, which no longer exists -- visibly broken rather than silently
    // rewritten. That is the deliberate half of the decision.
    bool wallBroken = false;
    for (const auto& row : c.parameters()) {
        if (row.name == "wall") wallBroken = !row.problem.empty();
    }
    CHECK(wallBroken);

    // One rename, one undo.
    REQUIRE(c.undo());
    CHECK(c.document().parameter("width") != nullptr);
    CHECK(c.document().parameter("plate_width") == nullptr);
}

TEST_CASE("a rename to a name already taken is refused", "[controller][parameters]") {
    app::Controller c;
    REQUIRE(c.setParameter("width", "40mm"));
    REQUIRE(c.setParameter("height", "20mm"));
    CHECK_FALSE(c.renameParameter("width", "height"));
    CHECK(c.parameters().size() == 2);
    CHECK(std::get<units::Length>(c.document().parameter("width")->value).base() == Approx(40.0));
}

TEST_CASE("removing a parameter breaks its users visibly", "[controller][parameters]") {
    // Rather than silently falling back to the number it last had, which would leave the model
    // looking correct and quietly no longer parametric.
    app::Controller c;
    REQUIRE(c.setParameter("width", "40mm"));
    const auto box = boxIn(c);
    REQUIRE(c.setProperty(box, "dx", "width * 2"));

    REQUIRE(c.removeParameter("width"));
    const auto object = c.document().find(box);
    REQUIRE(object);
    CHECK(object->state() == document::ObjectState::Failed);
    CHECK(object->error().message.find("width") != std::string::npos);
    CHECK(lengthOf(c, box, "dx") == Approx(80.0));   // last good value, not zero
}
