// A feature consumes its input, so the input is not drawn any more.
//
// Reported as "when I apply fillet the selected part looks like it's rendering incorrectly" — a
// dithered blue/grey checkerboard over the body. It was NOT a shader bug, and a spike measuring
// pixel colours proved the highlight lookup was clean (greys converted to blues with identical
// pixel counts, on a box and on a filleted box alike).
//
// The cause was here: `Controller::refresh` placed EVERY computed object, so a fillet's result and
// the box it was built from were both drawn, occupying the same space, and the two z-fought. The
// status bar said "3 mesh(es), 3 instances" for what a user saw as one body, which is the clue that
// was on screen the whole time. Selecting the box tinted its copy and the fight became
// blue-and-grey, which is how it was noticed at all.
//
// It is also simply what a history-based modeller means: SolidWorks and Inventor show the final
// body, not every intermediate one.

#include "cad/app/Controller.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

namespace {

/// The first object of a given type. A new part starts with three origin datum planes, so
/// "row zero is the body I just made" stopped being true — and was only ever true by accident.
cad::document::ObjectId firstOfType(cad::app::Controller& c, const std::string& type) {
    for (const auto& item : c.tree()) {
        if (item.type == type) return item.id;
    }
    return {};
}

}  // namespace


using namespace cad;

namespace {

/// Runs a registered command by id. Returns false if no such command, so a renamed id fails the
/// test rather than silently doing nothing.
bool run(app::Controller& c, const std::string& id) {
    for (const auto& command : c.commands()) {
        if (command.id == id) {
            if (command.invoke) command.invoke();
            return true;
        }
    }
    return false;
}

}  // namespace

TEST_CASE("a filleted box is drawn once, not twice", "[app][tip]") {
    app::Controller c;
    REQUIRE(run(c, "feature.box"));
    REQUIRE(c.stats().instances == 1);

    // Select the body, then fillet it. "Round every edge of the selected body" is what the command
    // does with no edges picked, which is the path the report came from.
    const document::ObjectId boxId = firstOfType(c, "Box");
    REQUIRE(boxId != document::ObjectId{});
    const std::size_t baseline = c.stats().objects;
    c.select(boxId, false);
    REQUIRE(run(c, "feature.fillet"));

    const auto after = c.stats();
    INFO("objects: " << after.objects << " instances: " << after.instances
                     << " failed: " << after.failed);
    REQUIRE(after.failed == 0);

    // ONE body. Two means the box and its filleted result are both being drawn in the same place,
    // which z-fights into a dither that looks exactly like a rendering bug.
    CHECK(after.instances == 1);
    // Both still IN the document; only one is drawn. Counted against a baseline rather than as an
    // absolute, so seeding another object into a new part does not break a test about drawing.
    CHECK(after.objects == baseline + 1);
}

TEST_CASE("a failed feature does not hide its input", "[app][tip]") {
    // The other half, and the more important one for a user. A dependent that FAILED consumes
    // nothing, so its input must stay visible — otherwise a fillet that cannot be built makes the
    // whole part vanish, which is the worst possible response to a failed operation.
    app::Controller c;
    REQUIRE(run(c, "feature.box"));

    const document::ObjectId boxId = firstOfType(c, "Box");
    REQUIRE(boxId != document::ObjectId{});
    c.select(boxId, false);
    REQUIRE(run(c, "feature.fillet"));

    // Push the radius past what the box can take, so the fillet fails.
    for (const auto& item : c.tree()) {
        if (item.type == "Fillet") {
            REQUIRE(c.setProperty(item.id, "radius", "5000 mm"));
            break;
        }
    }

    const auto after = c.stats();
    INFO("instances: " << after.instances << " failed: " << after.failed);
    REQUIRE(after.failed > 0);
    CHECK(after.instances == 1);   // the box is still there to look at
}

TEST_CASE("two independent bodies both stay visible", "[app][tip]") {
    // The case the old rule was right about, kept so the new rule cannot over-reach into hiding
    // bodies that nothing consumed.
    app::Controller c;
    REQUIRE(run(c, "feature.box"));
    REQUIRE(run(c, "feature.cylinder"));

    const auto after = c.stats();
    INFO("objects: " << after.objects << " instances: " << after.instances);
    CHECK(after.instances == 2);
}
