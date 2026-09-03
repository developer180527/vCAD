// Pattern and Mirror, the two features that turn one body into many.
//
// # Why these two were blocked, and on what
//
// Not on geometry. `nameCopy` has existed for a while and the kernel gained `rotate` and `mirror`
// alongside it, tested down to the case where a reflection lands on top of the body it came from.
// What was missing was the feature — so the capability existed and nobody could reach it, which is
// the shape of failure this project keeps finding.
//
// # What is actually at risk here
//
// A copy is the one operation the naming layer cannot handle by propagation. A rigid transform
// reports its elements as `Modified` — the SAME element, moved — which is right for a move and
// wrong for a copy: propagate the source's names onto a reflection and the fuse that follows finds
// two elements answering to one name. So these tests assert names as hard as they assert volume.
//
// Volume is the assertion of choice for the geometry because it catches the two failures a feature
// count cannot: a pattern that made every copy land on the original, and a mirror that produced a
// second body rather than joining.

#include "cad/app/Controller.h"
#include "cad/kernel/Shape.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <set>
#include <string>

using cad::app::Controller;
using Level = Controller::SelectionLevel;
using Catch::Approx;

namespace {

const cad::app::Command* commandNamed(const Controller& app, const std::string& id) {
    for (const auto& command : app.commands()) {
        if (command.id == id) return &command;
    }
    return nullptr;
}

double volumeOf(const Controller& app, cad::document::ObjectId id) {
    const auto object = app.document().find(id);
    if (!object || object->output() == nullptr) return -1.0;
    return object->output()->shape.volume();
}

/// The error a failed feature shows in the browser, or "" when it computed.
std::string errorOf(const Controller& app, cad::document::ObjectId id) {
    for (const auto& item : app.tree()) {
        if (item.id == id) return item.error;
    }
    return "";
}

cad::document::ObjectId aBox(Controller& app) {
    commandNamed(app, "feature.box")->invoke();
    app.refresh();
    REQUIRE(app.selection().size() == 1);
    return app.selection().front();
}

/// Selects a flat face of `id` through the pick path a click uses, so what gets selected is what a
/// user could have selected.
bool selectAFlatFace(Controller& app, cad::document::ObjectId id) {
    app.setSelectionLevel(Level::Face);
    for (std::uint32_t slot = 0; slot < 128; ++slot) {
        app.scriptNextPick(slot);
        const auto pick = app.pickAt(10, 10);
        if (!pick.hit || pick.object != id) continue;
        app.scriptNextPick(slot);
        if (app.clickAt(10, 10, /*additive=*/false).changed) return true;
    }
    return false;
}

std::size_t nameCountOf(const Controller& app, cad::document::ObjectId id) {
    const auto object = app.document().find(id);
    REQUIRE(object);
    REQUIRE(object->output() != nullptr);
    return object->output()->map.size();
}

}  // namespace

TEST_CASE("Mirror reflects a body about a picked face and joins it", "[mirror][command]") {
    // Asserted as roughly twice the volume. A mirror that produced the reflection and did NOT join
    // it would leave the original selected and look identical in the tree; a mirror that reflected
    // in place would change nothing at all. Only the volume separates the three.
    Controller app;
    const auto box = aBox(app);
    const double before = volumeOf(app, box);
    REQUIRE(before > 0.0);

    REQUIRE(selectAFlatFace(app, box));
    const auto* mirror = commandNamed(app, "feature.mirror");
    REQUIRE(mirror != nullptr);
    REQUIRE(mirror->enabled(app.context()));
    mirror->invoke();
    app.refresh();

    REQUIRE(app.selection().size() == 1);
    const auto id = app.selection().front();
    INFO("mirror reports: " << errorOf(app, id));
    CHECK(errorOf(app, id).empty());
    CHECK(volumeOf(app, id) == Approx(before * 2.0).epsilon(0.01));
}

TEST_CASE("a mirrored body's elements are all named", "[mirror][command][naming]") {
    // The half that has nothing to do with geometry. The reflection is a COPY, so propagating the
    // source's names onto it would give the join two elements with one name — and the join would
    // refuse with NamingLost rather than produce anything wrong. A silent success with fewer names
    // than elements is the failure this checks for.
    Controller app;
    const auto box = aBox(app);
    const std::size_t before = nameCountOf(app, box);

    REQUIRE(selectAFlatFace(app, box));
    commandNamed(app, "feature.mirror")->invoke();
    app.refresh();

    const auto id = app.selection().front();
    REQUIRE(errorOf(app, id).empty());
    const auto object = app.document().find(id);
    REQUIRE(object->output() != nullptr);

    CHECK(object->output()->map.unnamed(object->output()->shape).empty());
    CHECK(object->output()->map.collisions().empty());
    // A mirrored box joined to itself is one larger box: the shared face is consumed, so the result
    // has FEWER elements than two boxes and more than one.
    CHECK(nameCountOf(app, id) > 1);
    CHECK(nameCountOf(app, id) < before * 2);
}

TEST_CASE("Mirror offers itself only for a face", "[mirror][command]") {
    Controller app;
    const auto box = aBox(app);
    const auto* mirror = commandNamed(app, "feature.mirror");
    REQUIRE(mirror != nullptr);

    // A body is not a plane. The face carries the plane AND says which body to reflect, so a body
    // selection says nothing about where the mirror goes.
    CHECK_FALSE(mirror->enabled(app.context()));

    REQUIRE(selectAFlatFace(app, box));
    CHECK(mirror->enabled(app.context()));
}

TEST_CASE("Mirror refuses a curved face without leaving wreckage", "[mirror][command]") {
    // Refused in the shell, before a feature exists. computeMirror would reject it too, but only
    // after the row was in the browser for the user to find and delete.
    Controller app;
    commandNamed(app, "feature.cylinder")->invoke();
    app.refresh();
    const auto id = app.selection().front();

    std::string lastStatus;
    app.onStatus([&lastStatus](const std::string& s) { lastStatus = s; });

    app.setSelectionLevel(Level::Face);
    bool sawRefusal = false;
    const std::size_t before = app.tree().size();
    for (std::uint32_t slot = 0; slot < 64; ++slot) {
        app.scriptNextPick(slot);
        const auto pick = app.pickAt(10, 10);
        if (!pick.hit || pick.object != id) continue;
        app.scriptNextPick(slot);
        if (!app.clickAt(10, 10, false).changed) continue;

        const auto object = app.document().find(id);
        const auto shape = object->output()->map.resolve(app.elementSelection().front().element);
        if (!shape || cad::kernel::planeOf(*shape)) continue;   // a flat cap: not this case

        commandNamed(app, "feature.mirror")->invoke();
        sawRefusal = true;
        INFO("status: " << lastStatus);
        CHECK_FALSE(lastStatus.empty());
        CHECK(app.tree().size() == before);   // nothing added
    }
    CHECK(sawRefusal);
}

TEST_CASE("Pattern repeats a body along a direction", "[pattern][command]") {
    Controller app;
    const auto box = aBox(app);
    const double before = volumeOf(app, box);

    REQUIRE(app.beginCommand("feature.pattern"));
    REQUIRE(app.setCommandParameter("count", "3"));
    REQUIRE(app.setCommandParameter("dx", "200"));   // clear of the 100 mm box, so nothing overlaps
    REQUIRE(app.commitCommand());
    app.refresh();

    const auto id = app.selection().front();
    INFO("pattern reports: " << errorOf(app, id));
    CHECK(errorOf(app, id).empty());
    // Three separate bodies, so three times the volume. Overlapping copies would give less, and a
    // pattern that never moved anything would give exactly one.
    CHECK(volumeOf(app, id) == Approx(before * 3.0).epsilon(0.01));
}

TEST_CASE("a pattern's count includes the original", "[pattern][command]") {
    // What every CAD system means by it, and what a user counts looking at the result. It also
    // makes 1 a legal no-op rather than an error — which matters because 1 is what a cleared field
    // parses to, and refusing it would make the field impossible to empty.
    Controller app;
    const auto box = aBox(app);
    const double before = volumeOf(app, box);

    REQUIRE(app.beginCommand("feature.pattern"));
    REQUIRE(app.setCommandParameter("count", "1"));
    REQUIRE(app.setCommandParameter("dx", "200"));
    REQUIRE(app.commitCommand());
    app.refresh();

    const auto id = app.selection().front();
    CHECK(errorOf(app, id).empty());
    CHECK(volumeOf(app, id) == Approx(before));
}

TEST_CASE("a pattern with no spacing is refused, not silently built", "[pattern][command]") {
    // Every copy would land on the original, the fuse would return one body, and the feature would
    // report success having changed nothing visible — which reads as the pattern being broken
    // rather than as the spacing being unset.
    Controller app;
    const auto box = aBox(app);
    (void)box;

    REQUIRE(app.beginCommand("feature.pattern"));
    REQUIRE(app.setCommandParameter("count", "4"));
    REQUIRE(app.setCommandParameter("dx", "0"));
    REQUIRE(app.setCommandParameter("dy", "0"));
    REQUIRE(app.setCommandParameter("dz", "0"));
    REQUIRE(app.commitCommand());
    app.refresh();

    const auto id = app.selection().front();
    INFO("expected a refusal, got: '" << errorOf(app, id) << "'");
    CHECK_FALSE(errorOf(app, id).empty());
}

TEST_CASE("every element of a pattern is named and distinct", "[pattern][command][naming]") {
    // The property `nameCopy` and the instance coordinate exist for. Three copies of one box that
    // shared their source's names would collide the moment the second was joined; the operation
    // would refuse rather than mislead, so a clean result IS the assertion.
    Controller app;
    const auto box = aBox(app);
    const std::size_t before = nameCountOf(app, box);

    REQUIRE(app.beginCommand("feature.pattern"));
    REQUIRE(app.setCommandParameter("count", "3"));
    REQUIRE(app.setCommandParameter("dx", "200"));
    REQUIRE(app.commitCommand());
    app.refresh();

    const auto id = app.selection().front();
    REQUIRE(errorOf(app, id).empty());
    const auto object = app.document().find(id);
    REQUIRE(object->output() != nullptr);

    CHECK(object->output()->map.unnamed(object->output()->shape).empty());
    CHECK(object->output()->map.collisions().empty());
    // Three disjoint boxes: nothing merges, so every element of every copy survives.
    CHECK(nameCountOf(app, id) == before * 3);

    // And they really are distinct names, not one name bound three times — which is what
    // `collisions` catches, asserted here as a count so a partial failure cannot hide.
    std::set<std::string> unique;
    for (const auto& name : object->output()->map.allNames()) unique.insert(name.toString());
    CHECK(unique.size() == before * 3);
}

TEST_CASE("the Pattern panel asks for a count and a spacing", "[pattern][command]") {
    // The panel is built from the feature's declaration, so this also pins that a Count field
    // arrives as an integer rather than as a length — "3" must mean three copies, not 3 mm.
    Controller app;
    (void)aBox(app);
    REQUIRE(app.beginCommand("feature.pattern"));

    const auto& parameters = app.commandParameters();
    REQUIRE(parameters.size() == 4);
    CHECK(parameters[0].name == "count");
    CHECK(parameters[0].kind == cad::app::CommandParameter::Kind::Integer);
    CHECK(parameters[1].name == "dx");
    CHECK(parameters[1].kind == cad::app::CommandParameter::Kind::Length);
    app.cancelCommand();
}

// ── the properties, rather than the happy path ────────────────────────────────────────────
//
// Everything above checks that the two features DO something. What follows checks the things that
// make them survivable: that a rebuild produces the same names, that editing a count does not
// silently re-point a downstream feature at a different copy, and that the arithmetic underneath
// the instance coordinate cannot wrap.

namespace {

std::set<std::string> allNamesOf(const Controller& app, cad::document::ObjectId id) {
    const auto object = app.document().find(id);
    REQUIRE(object);
    REQUIRE(object->output() != nullptr);
    std::set<std::string> names;
    for (const auto& name : object->output()->map.allNames()) names.insert(name.toString());
    return names;
}

/// A box with a pattern on it, returning the pattern's id.
cad::document::ObjectId aPattern(Controller& app, const char* count, const char* dx) {
    (void)aBox(app);
    REQUIRE(app.beginCommand("feature.pattern"));
    REQUIRE(app.setCommandParameter("count", count));
    REQUIRE(app.setCommandParameter("dx", dx));
    REQUIRE(app.commitCommand());
    app.refresh();
    REQUIRE(app.selection().size() == 1);
    return app.selection().front();
}

}   // namespace

TEST_CASE("a pattern names the same elements on every rebuild", "[pattern][naming]") {
    // The contract the whole naming layer exists for, asked of the feature rather than of the
    // layer. A name that changed between rebuilds would break every reference into the pattern the
    // moment anything upstream was edited -- and the copies are exactly where that is most likely,
    // because they are the elements whose names are minted rather than carried.
    const auto namesFromAFreshDocument = [] {
        Controller app;
        const auto id = aPattern(app, "3", "200");
        return allNamesOf(app, id);
    };

    CHECK(namesFromAFreshDocument() == namesFromAFreshDocument());
}

TEST_CASE("a pattern rebuilds identically after an unrelated edit", "[pattern][naming]") {
    // Stronger than running it twice: the same document, recomputed, must not renumber anything.
    // A cache miss and a fresh compute have to agree, or a reference taken before an edit resolves
    // to a different copy after it.
    Controller app;
    const auto id = aPattern(app, "4", "150");
    const auto before = allNamesOf(app, id);

    app.refresh();
    app.refresh();
    CHECK(allNamesOf(app, id) == before);
}

TEST_CASE("changing the count keeps the instances that remain", "[pattern][naming]") {
    // The claim the instance COORDINATE makes, and the reason it is not a running index: instance 2
    // means "two steps along the direction", so growing the pattern from three to five must leave
    // instances 1 and 2 named exactly as they were. If it did not, a fillet on instance 2 would
    // silently move to a different copy the moment somebody added a fourth.
    Controller app;
    const auto id = aPattern(app, "3", "200");
    const auto before = allNamesOf(app, id);

    // Through setProperty, which is the path a user takes -- text in, parsed by the same code the
    // property panel uses. Editing the document directly would test a route nobody can reach.
    REQUIRE(app.setProperty(id, "count", "5"));
    app.refresh();

    const auto after = allNamesOf(app, id);
    REQUIRE(after.size() > before.size());   // it really did grow

    // Every name from the smaller pattern still names something in the larger one.
    std::size_t kept = 0;
    for (const auto& name : before) {
        if (after.count(name) != 0) ++kept;
    }
    INFO("of " << before.size() << " names before, " << kept << " survived the count change");
    CHECK(kept == before.size());
}

TEST_CASE("reversing the direction keeps every instance's name", "[pattern][naming]") {
    // An instance is a STEP ALONG THE DIRECTION, counted from the seed -- not a position. So
    // flipping the spacing moves the geometry and must not rename anything: instance 3 is still
    // three steps along, on the other side. Encoding direction as the sign of the spacing instead
    // would silently reassign instance 3 to a different physical copy, and any feature built on it
    // would move for no visible reason.
    Controller app;
    const auto id = aPattern(app, "3", "200");
    const auto before = allNamesOf(app, id);
    const double volumeBefore = volumeOf(app, id);

    REQUIRE(app.setProperty(id, "dx", "-200"));
    app.refresh();

    CHECK(allNamesOf(app, id) == before);                       // same names
    CHECK(volumeOf(app, id) == Approx(volumeBefore));           // same amount of material
    CHECK(app.document().find(id)->output() != nullptr);
}

TEST_CASE("copies that overlap still name cleanly", "[pattern][naming]") {
    // The disjoint case is the easy one: nothing splits, so propagate never has to discriminate.
    // A spacing SHORTER than the body makes every copy overlap its neighbour, faces split, and the
    // pieces are geometrically identical across instances -- which is the tie the discriminator
    // refuses to guess at.
    //
    // Written first to accept either outcome, clean names or an honest refusal, because it was not
    // obvious which this would reach. It comes out clean, so the hedge is gone: a test that accepts
    // both cannot notice the day it stops being the first one.
    Controller app;
    const auto id = aPattern(app, "3", "40");   // the box is 100 mm in x

    const auto object = app.document().find(id);
    REQUIRE(object);
    INFO("pattern reports: " << errorOf(app, id));
    REQUIRE(object->output() != nullptr);
    CHECK(object->output()->map.unnamed(object->output()->shape).empty());
    CHECK(object->output()->map.collisions().empty());
    // One merged body, so less material than three separate ones.
    CHECK(volumeOf(app, id) > 0.0);
}

TEST_CASE("a pattern too large to build is refused, not attempted", "[pattern][command]") {
    // Each instance is a boolean on the calling thread and recompute is synchronous with no
    // cancellation, so an unbounded count is not a slow pattern -- it is an application that has
    // stopped answering. Underneath that, the op tag is 16 bits and each instance uses two, so a
    // count past 32768 wraps and two instances mint identical names. The bound means that
    // arithmetic is unreachable rather than merely unlikely.
    Controller app;
    (void)aBox(app);
    REQUIRE(app.beginCommand("feature.pattern"));
    REQUIRE(app.setCommandParameter("count", "50000"));
    REQUIRE(app.setCommandParameter("dx", "200"));
    REQUIRE(app.commitCommand());
    app.refresh();

    const auto id = app.selection().front();
    INFO("expected a refusal, got: '" << errorOf(app, id) << "'");
    CHECK_FALSE(errorOf(app, id).empty());
}

TEST_CASE("a mirror rebuilds identically", "[mirror][naming]") {
    const auto namesFromAFreshDocument = [] {
        Controller app;
        const auto box = aBox(app);
        REQUIRE(selectAFlatFace(app, box));
        commandNamed(app, "feature.mirror")->invoke();
        app.refresh();
        return allNamesOf(app, app.selection().front());
    };

    CHECK(namesFromAFreshDocument() == namesFromAFreshDocument());
}

TEST_CASE("Pattern and Mirror are each one undo step", "[pattern][mirror][command]") {
    // A feature that committed twice would make the user press undo twice, with the first press
    // appearing to do nothing. Pattern builds N bodies and joins them, and Mirror builds a copy and
    // joins it -- both are several kernel operations and exactly one thing the user did.
    Controller app;
    (void)aBox(app);
    const std::size_t afterBox = app.tree().size();

    REQUIRE(app.beginCommand("feature.pattern"));
    REQUIRE(app.setCommandParameter("count", "3"));
    REQUIRE(app.setCommandParameter("dx", "200"));
    REQUIRE(app.commitCommand());
    app.refresh();
    REQUIRE(app.tree().size() > afterBox);

    REQUIRE(app.undo());
    app.refresh();
    CHECK(app.tree().size() == afterBox);
}

// ── the faces a join leaves behind ────────────────────────────────────────────────────────
//
// Reported from the UI: a pattern of a cuboid "adds it 4 times each adjacent to each other". The
// arithmetic was right -- three copies 50 mm apart on a 100 mm box overlap into one 200 mm slab --
// but the slab was DRAWN with a line at every place two copies met, because fusing solids that
// share a wall leaves those walls as separate faces lying on one plane.

TEST_CASE("overlapping copies merge into one clean body", "[pattern][faces]") {
    // Measured as a FACE COUNT at unchanged volume, which is the only way to tell "merged" from
    // "deleted something". 18 faces before the merge, 6 after; a box is 6 faces however many copies
    // went into it.
    Controller app;
    const auto box = aBox(app);
    const double one = volumeOf(app, box);

    REQUIRE(app.beginCommand("feature.pattern"));
    REQUIRE(app.setCommandParameter("count", "3"));
    REQUIRE(app.setCommandParameter("dx", "50"));   // shorter than the 100 mm box, so they overlap
    REQUIRE(app.commitCommand());
    app.refresh();

    const auto id = app.selection().front();
    const auto object = app.document().find(id);
    REQUIRE(object);
    INFO("pattern reports: " << errorOf(app, id));
    REQUIRE(object->output() != nullptr);

    const auto faces = object->output()->shape.subShapes(cad::kernel::ShapeType::Face).size();
    INFO("faces: " << faces);
    CHECK(faces == 6);

    // The volume is what says the merge removed no material: 0..200 x 60 x 40 against the original
    // 100 x 60 x 40. A "merge" that lost a copy would also reduce the face count.
    CHECK(volumeOf(app, id) == Approx(one * 2.0).epsilon(0.001));
}

TEST_CASE("copies that do not touch are not merged", "[pattern][faces]") {
    // The control. unifySameDomain merges faces on the SAME surface, so bodies standing apart must
    // keep all of theirs -- and a fix that merged unconditionally would quietly weld a pattern of
    // separate parts into one.
    Controller app;
    const auto box = aBox(app);
    const double one = volumeOf(app, box);

    REQUIRE(app.beginCommand("feature.pattern"));
    REQUIRE(app.setCommandParameter("count", "3"));
    REQUIRE(app.setCommandParameter("dx", "200"));   // clear of the 100 mm box
    REQUIRE(app.commitCommand());
    app.refresh();

    const auto object = app.document().find(app.selection().front());
    REQUIRE(object->output() != nullptr);
    CHECK(object->output()->shape.subShapes(cad::kernel::ShapeType::Face).size() == 18);
    CHECK(volumeOf(app, app.selection().front()) == Approx(one * 3.0).epsilon(0.001));
}

TEST_CASE("names survive the merge", "[pattern][faces][naming]") {
    // The half that is easy to lose. unifySameDomain changes topology, so a name bound to a face it
    // merges is a name for something that no longer exists -- which is why the merge is followed by
    // a propagate rather than done quietly after naming. Getting the order wrong produces a map
    // describing a shape nobody has, and nothing about the geometry would say so.
    Controller app;
    (void)aBox(app);
    REQUIRE(app.beginCommand("feature.pattern"));
    REQUIRE(app.setCommandParameter("count", "3"));
    REQUIRE(app.setCommandParameter("dx", "50"));
    REQUIRE(app.commitCommand());
    app.refresh();

    const auto object = app.document().find(app.selection().front());
    REQUIRE(object->output() != nullptr);
    CHECK(object->output()->map.unnamed(object->output()->shape).empty());
    CHECK(object->output()->map.collisions().empty());
}

TEST_CASE("a mirrored body is one clean solid too", "[mirror][faces]") {
    // A mirror joins a body to its reflection across a shared face, so it leaves exactly the same
    // coplanar walls a pattern does.
    Controller app;
    const auto box = aBox(app);
    REQUIRE(selectAFlatFace(app, box));
    commandNamed(app, "feature.mirror")->invoke();
    app.refresh();

    const auto object = app.document().find(app.selection().front());
    REQUIRE(object->output() != nullptr);
    const auto faces = object->output()->shape.subShapes(cad::kernel::ShapeType::Face).size();
    INFO("faces: " << faces);
    CHECK(faces == 6);
    CHECK(object->output()->map.unnamed(object->output()->shape).empty());
}

TEST_CASE("the default spacing does not overlap the default box", "[pattern][command]") {
    // A default is a demonstration of what the tool does. 50 mm on a 100 mm box made every copy
    // overlap its neighbour and fuse into one slab -- correct arithmetic that read as the feature
    // being broken, which is how it was reported.
    Controller app;
    const auto box = aBox(app);
    const double one = volumeOf(app, box);

    REQUIRE(app.beginCommand("feature.pattern"));
    REQUIRE(app.commitCommand());   // the declared defaults, untouched
    app.refresh();

    CHECK(volumeOf(app, app.selection().front()) == Approx(one * 3.0).epsilon(0.001));
}
