/// A feature says what it needs, once.
///
/// # The bug shape this closes
///
/// A feature type used to declare a name, a version and a compute function, and nothing about its
/// own inputs. Four places then had to agree independently on what "Hole" requires: the command's
/// enable predicate, the panel that lists its fields, the feature's own guard, and the property
/// names all three read and write.
///
/// They drifted, and the failure was silent in the worst direction: Hole and Revolve both computed
/// correctly and were UNREACHABLE, greyed out with exactly the right thing selected, because the
/// predicate and the implementation had come to disagree about what "selected" meant. That was the
/// fourth fix of the same shape.
///
/// So these tests are mostly about the four places giving ONE answer -- not about any individual
/// feature's rule being right.

#include "cad/app/Controller.h"
#include "cad/features/Builtins.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace cad;

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

bool enabled(app::Controller& c, const std::string& id) {
    for (const auto& command : c.commands()) {
        if (command.id == id) return !command.enabled || command.enabled(c.context());
    }
    return false;
}

}   // namespace

TEST_CASE("every built-in that takes values declares them", "[features][inputs]") {
    // The declaration is what the panel is built from, so a feature that forgot to declare its
    // values would open an empty panel and commit whatever the defaults happened to be.
    const auto registry = features::builtins();
    for (const char* name : {"Box", "Cylinder", "Extrude", "Revolve", "Hole", "Translate"}) {
        const auto* type = registry.find(name);
        INFO(name);
        REQUIRE(type != nullptr);
        CHECK_FALSE(type->inputs.values.empty());
    }
}

TEST_CASE("a feature that needs a selection says what it needs", "[features][inputs]") {
    // One wording, in one place. Three separate copies of "Select one flat face..." is how they
    // come to differ, and a refusal that does not match the tooltip is a refusal the user distrusts.
    const auto registry = features::builtins();
    for (const char* name : {"Extrude", "Revolve", "Hole", "Translate", "Cut"}) {
        const auto* type = registry.find(name);
        INFO(name);
        REQUIRE(type != nullptr);
        REQUIRE_FALSE(type->inputs.accepts.empty());
        CHECK_FALSE(type->inputs.prompt.empty());
    }

    // And a primitive needs nothing, so it must not claim to.
    for (const char* name : {"Box", "Cylinder"}) {
        const auto* type = registry.find(name);
        REQUIRE(type != nullptr);
        INFO(name);
        CHECK(type->inputs.accepts.empty());
    }
}

TEST_CASE("the button, the panel and the guard give one answer", "[features][inputs]") {
    // The actual regression. With nothing selected, all three must refuse; with the right thing
    // selected, all three must agree that it is fine. Any two of them disagreeing is the bug that
    // left Hole unreachable.
    app::Controller c;
    const auto box = boxIn(c);
    const auto faces = elementsOfType(c, box, kernel::ShapeType::Face);
    REQUIRE_FALSE(faces.empty());

    c.clearSelection();
    CHECK_FALSE(enabled(c, "feature.hole"));                    // the button
    CHECK_FALSE(c.beginCommand("feature.hole"));                // the panel
    CHECK_FALSE(c.selectionShortfall("Hole").empty());          // the guard

    c.setSelectionLevel(app::Controller::SelectionLevel::Auto);
    c.selectElement(box, faces[0], false);
    CHECK(enabled(c, "feature.hole"));
    CHECK(c.selectionShortfall("Hole").empty());
    CHECK(c.beginCommand("feature.hole"));
    c.cancelCommand();
}

TEST_CASE("the panel is built from the declaration", "[features][inputs]") {
    // Field names are the PROPERTY names the compute reads. A panel that invented its own would
    // collect values the feature never sees -- which looks like the feature ignoring the user.
    app::Controller c;
    const auto box = boxIn(c);
    const auto faces = elementsOfType(c, box, kernel::ShapeType::Face);
    REQUIRE_FALSE(faces.empty());
    c.selectElement(box, faces[0], false);

    REQUIRE(c.beginCommand("feature.hole"));
    const auto fields = c.commandParameters();
    REQUIRE(fields.size() == 2);
    CHECK(fields[0].name == "diameter");
    CHECK(fields[1].name == "depth");
    CHECK(fields[0].label == "Diameter");

    // And every declared field reaches the panel, in the order declared.
    const auto registry = features::builtins();
    const auto* hole = registry.find("Hole");
    REQUIRE(hole != nullptr);
    REQUIRE(hole->inputs.values.size() == fields.size());
    for (std::size_t i = 0; i < fields.size(); ++i) {
        CHECK(fields[i].name == hole->inputs.values[i].name);
    }
    c.cancelCommand();
}

TEST_CASE("an owner type is part of the requirement", "[features][inputs]") {
    // Revolve needs an edge OF A SKETCH, because the axis is resolved in the profile's own element
    // map. An edge of a box satisfies "one edge" and is still the wrong answer.
    app::Controller c;
    const auto box = boxIn(c);
    const auto edges = elementsOfType(c, box, kernel::ShapeType::Edge);
    REQUIRE_FALSE(edges.empty());

    c.setSelectionLevel(app::Controller::SelectionLevel::Auto);
    c.selectElement(box, edges[0], false);

    INFO(c.selectionShortfall("Revolve"));
    CHECK_FALSE(c.selectionShortfall("Revolve").empty());   // one edge, wrong owner
    CHECK_FALSE(enabled(c, "feature.revolve"));
}

TEST_CASE("a count that is too high is refused as well as one too low", "[features][inputs]") {
    // Hole takes exactly one face. Two faces is two holes, which is a reasonable thing to want and
    // not a reasonable thing to guess -- so the upper bound has to be enforced, not just the lower.
    app::Controller c;
    const auto box = boxIn(c);
    const auto faces = elementsOfType(c, box, kernel::ShapeType::Face);
    REQUIRE(faces.size() >= 2);

    c.setSelectionLevel(app::Controller::SelectionLevel::Auto);
    c.selectElement(box, faces[0], false);
    REQUIRE(c.selectionShortfall("Hole").empty());

    c.selectElement(box, faces[1], true);
    CHECK_FALSE(c.selectionShortfall("Hole").empty());
    CHECK_FALSE(enabled(c, "feature.hole"));
}

TEST_CASE("an unbounded requirement accepts many", "[features][inputs]") {
    // Fillet declares edges with no upper bound, so picking six of them is not "too many".
    app::Controller c;
    const auto box = boxIn(c);
    const auto edges = elementsOfType(c, box, kernel::ShapeType::Edge);
    REQUIRE(edges.size() >= 6);

    c.setSelectionLevel(app::Controller::SelectionLevel::Auto);
    for (std::size_t i = 0; i < 6; ++i) c.selectElement(box, edges[i], i != 0);
    CHECK(c.selectionShortfall("Fillet").empty());
    CHECK(enabled(c, "feature.fillet"));
}
