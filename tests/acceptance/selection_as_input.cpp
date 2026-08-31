/// Selection as an input to features.
///
/// # The bug this closes, three times over
///
/// Every feature that takes geometry needs the same answer — which faces, which edges, which bodies
/// — and each one used to work it out for itself by walking the element selection and checking
/// `selectionLevel()`. Those level tests were written when Edge and Face were modes you switched
/// into. The day Auto became the default they all stopped being true, and three features broke in
/// two different ways:
///
///   * Fillet and Chamfer fell through to "every edge of the body", rounding all twelve edges of a
///     box when two were picked.
///   * Hole and Revolve greyed out with exactly the right thing selected, and could not be run at
///     all.
///
/// The level says what a CLICK RESOLVES TO. It does not describe what is already selected. These
/// tests pin that distinction at the level a user experiences it: with Auto on, picking the right
/// geometry must enable and drive the right feature.

#include "cad/app/Controller.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

using namespace cad;

namespace {

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

document::ObjectId boxIn(app::Controller& c) {
    REQUIRE(c.beginCommand("feature.box"));
    REQUIRE(c.commitCommand());
    return c.selection().front();
}

}   // namespace

TEST_CASE("the summary sorts a selection by what it actually is", "[selection][inputs]") {
    app::Controller c;
    const auto box = boxIn(c);
    const auto faces = elementsOfType(c, box, kernel::ShapeType::Face);
    const auto edges = elementsOfType(c, box, kernel::ShapeType::Edge);
    REQUIRE_FALSE(faces.empty());
    REQUIRE_FALSE(edges.empty());

    c.setSelectionLevel(app::Controller::SelectionLevel::Auto);
    c.selectElement(box, faces[0], false);
    c.selectElement(box, edges[0], true);

    const auto summary = c.selectionByKind();
    CHECK(summary.faces.size() == 1);
    CHECK(summary.edges.size() == 1);
    CHECK(summary.vertices.empty());
    CHECK(summary.oneOwner());
    CHECK(summary.owner() == box);
}

TEST_CASE("Hole is available with a face selected under Auto", "[selection][inputs]") {
    // It was not: the predicate asked for Face LEVEL, so with Auto on — the default — Hole was
    // greyed out however carefully the user picked a face.
    app::Controller c;
    const auto box = boxIn(c);
    const auto faces = elementsOfType(c, box, kernel::ShapeType::Face);
    REQUIRE_FALSE(faces.empty());

    c.setSelectionLevel(app::Controller::SelectionLevel::Auto);
    CHECK_FALSE(enabled(c, "feature.hole"));   // nothing picked yet

    c.selectElement(box, faces[0], false);
    CHECK(enabled(c, "feature.hole"));
}

TEST_CASE("Fillet takes the picked edges under Auto, not all of them", "[selection][inputs]") {
    app::Controller c;
    const auto box = boxIn(c);
    const auto edges = elementsOfType(c, box, kernel::ShapeType::Edge);
    REQUIRE(edges.size() >= 4);

    c.setSelectionLevel(app::Controller::SelectionLevel::Auto);
    c.selectElement(box, edges[0], false);
    c.selectElement(box, edges[1], true);
    REQUIRE(enabled(c, "feature.fillet"));

    std::string said;
    c.onStatus([&](const std::string& text) { said = text; });
    for (const auto& command : c.commands()) {
        if (command.id == "feature.fillet" && command.invoke) { command.invoke(); break; }
    }
    INFO("status: " << said);
    // Twelve is what "every edge of a box" means, and the message names the count.
    CHECK(said.find("12") == std::string::npos);
}

TEST_CASE("edges from two bodies are refused rather than half used", "[selection][inputs]") {
    // A fillet takes a base shape and edges OF it. Edges from two bodies is not one feature with a
    // strange input — it is two features, and picking one silently drops half the selection.
    app::Controller c;
    const auto first = boxIn(c);
    const auto second = boxIn(c);
    const auto a = elementsOfType(c, first, kernel::ShapeType::Edge);
    const auto b = elementsOfType(c, second, kernel::ShapeType::Edge);
    REQUIRE_FALSE(a.empty());
    REQUIRE_FALSE(b.empty());

    c.setSelectionLevel(app::Controller::SelectionLevel::Auto);
    c.selectElement(first, a[0], false);
    c.selectElement(second, b[0], true);

    const auto summary = c.selectionByKind();
    CHECK(summary.edges.size() == 2);
    CHECK_FALSE(summary.oneOwner());

    std::string said;
    c.onStatus([&](const std::string& text) { said = text; });
    for (const auto& command : c.commands()) {
        if (command.id == "feature.fillet" && command.invoke) { command.invoke(); break; }
    }
    INFO("status: " << said);
    CHECK(said.find("one body at a time") != std::string::npos);
}

TEST_CASE("the command context counts by kind", "[selection][inputs]") {
    // What the shells' buttons are enabled from. Counting elements alone cannot express "one face",
    // which is why every feature used to reach past the context for the level.
    app::Controller c;
    const auto box = boxIn(c);
    const auto faces = elementsOfType(c, box, kernel::ShapeType::Face);
    const auto edges = elementsOfType(c, box, kernel::ShapeType::Edge);
    REQUIRE_FALSE(faces.empty());
    REQUIRE(edges.size() >= 2);

    c.selectElement(box, edges[0], false);
    c.selectElement(box, edges[1], true);
    auto ctx = c.context();
    CHECK(ctx.selectedEdges == 2);
    CHECK(ctx.selectedFaces == 0);

    c.selectElement(box, faces[0], false);
    ctx = c.context();
    CHECK(ctx.selectedFaces == 1);
    CHECK(ctx.selectedEdges == 0);
}
