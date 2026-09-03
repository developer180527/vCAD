// Two identical features are two features.
//
// # The bug
//
// `Engine::cacheKeyOf` mixed in the feature type, its version, every non-cosmetic property and any
// external inputs -- and not the object's own id. But that id IS `ComputeContext::namingSerial`,
// and the compute stamps it into every element name it produces. So the key omitted one of its own
// inputs, which is exactly the cooking-pipeline mistake the same function guards against two lines
// earlier for the feature version.
//
// What that did: two Box features with identical dx/dy/dz hashed alike, and the second was handed
// the FIRST one's cached Output -- shape and ElementMap together. All 26 element names came back
// identical, both carrying serial 4 while the object ids were 4 and 5.
//
// # Why it mattered more than a duplicate
//
// It surfaced as every boolean between two identical bodies failing with "Two pieces of this shape
// ended up with the same identity" -- which reads as a limitation of the naming scheme and is not
// one. Duplicating a part and joining it is ordinary modelling: two identical brackets, a mirrored
// half welded to its original.
//
// And the refusal was the good outcome. Underneath it, a reference to one body's face genuinely
// resolved in the other, and `ElementMap::collisions` is the only reason that showed up as an error
// rather than as a fillet landing on the wrong body.
//
// # What is asserted
//
// The property, not the symptom: two features that differ ONLY by identity must not share element
// names. The boolean succeeding is the consequence, and it is checked second so a failure here
// says which of the two broke.

#include "cad/app/Controller.h"
#include "cad/kernel/Shape.h"

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>

using cad::app::Controller;

namespace {

void invokeCommand(Controller& app, const std::string& id) {
    for (const auto& command : app.commands()) {
        if (command.id != id) continue;
        REQUIRE(command.invoke);
        command.invoke();
        app.refresh();
        return;
    }
    FAIL("no such command: " << id);
}

/// Creates a box with the DEFAULT dimensions, so two calls differ by nothing but identity.
cad::document::ObjectId aBox(Controller& app) {
    invokeCommand(app, "feature.box");
    REQUIRE(app.selection().size() == 1);
    return app.selection().front();
}

std::set<std::string> namesOf(const Controller& app, cad::document::ObjectId id) {
    const auto object = app.document().find(id);
    REQUIRE(object);
    REQUIRE(object->output() != nullptr);
    std::set<std::string> names;
    for (const auto& name : object->output()->map.allNames()) names.insert(name.toString());
    return names;
}

}  // namespace

TEST_CASE("two identical bodies do not share element names", "[cache][naming]") {
    // The property itself. Asserted as a set intersection rather than by counting, because the
    // failure was TOTAL -- every one of the 26 names was shared -- and a test that allowed "mostly
    // different" would pass on a fix that only half worked.
    Controller app;
    const auto first = aBox(app);
    const auto second = aBox(app);
    REQUIRE(first != second);

    const auto a = namesOf(app, first);
    const auto b = namesOf(app, second);
    REQUIRE_FALSE(a.empty());
    REQUIRE(a.size() == b.size());   // the same SHAPE, so the same number of elements

    std::size_t shared = 0;
    for (const auto& name : a) {
        if (b.count(name) != 0) ++shared;
    }
    INFO("of " << a.size() << " names on the first body, " << shared
               << " also name an element of the second");
    CHECK(shared == 0);
}

TEST_CASE("a body's own name survives a second identical body appearing", "[cache][naming]") {
    // The half that says the fix went the right way. Making the SECOND body different would also
    // be achievable by renaming the first, which would break every reference taken before it -- the
    // one thing the naming layer must never do casually.
    Controller app;
    const auto first = aBox(app);
    const auto before = namesOf(app, first);

    (void)aBox(app);
    const auto after = namesOf(app, first);
    CHECK(before == after);
}

TEST_CASE("two identical bodies can be joined", "[cache][naming][boolean]") {
    // The symptom, checked second. Duplicating a part and joining it is ordinary modelling, and
    // this failed for every pair of geometrically identical bodies.
    Controller app;
    const auto first = aBox(app);
    const auto second = aBox(app);

    // Moved so the fuse has something to do; the bug was not about coincident geometry, and this
    // failed just as reliably with the second body offset.
    app.select(second, /*additive=*/false);
    REQUIRE(app.beginCommand("feature.translate"));
    REQUIRE(app.setCommandParameter("dx", "50"));
    REQUIRE(app.commitCommand());
    app.refresh();
    const auto moved = app.selection().front();

    app.select(first, /*additive=*/false);
    app.select(moved, /*additive=*/true);
    invokeCommand(app, "feature.fuse");

    REQUIRE(app.selection().size() == 1);
    const auto fused = app.selection().front();
    std::string error;
    for (const auto& item : app.tree()) {
        if (item.id == fused) error = item.error;
    }
    INFO("the fused feature reports: " << error);
    CHECK(error.empty());

    const auto object = app.document().find(fused);
    REQUIRE(object);
    REQUIRE(object->output() != nullptr);
    CHECK(object->output()->shape.volume() > 0.0);
}
