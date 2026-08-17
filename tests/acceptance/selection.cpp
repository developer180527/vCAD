// Clicking geometry to select it.
//
// The largest usability gap the feature audit found, and the reason it is worth its own file: before
// this, `Controller::select` was reachable from exactly one place in the whole shell -- the model
// tree. A click in the 3D viewport mapped to an orbit gesture and returned, so nothing a user could
// see was clickable.
//
// Headless, through the scripted null picker, for the same reason face_pick.cpp is: what this layer
// owns is what a click MEANS -- which level it resolves at, what it toggles, what it says when it
// cannot do anything -- and none of that needs a GPU. Whether the id buffer holds the right ids does,
// and is not claimed here.

#include "cad/app/Controller.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

using cad::app::Controller;
using Level = Controller::SelectionLevel;

namespace {

cad::document::ObjectId create(Controller& app, const std::string& commandId) {
    for (const auto& command : app.commands()) {
        if (command.id != commandId) continue;
        command.invoke();
        break;
    }
    app.refresh();
    REQUIRE(app.selection().size() == 1);
    return app.selection().front();
}

/// The element slots the scene holds for `id` that a click at `level` accepts.
///
/// Found by asking, rather than by consulting the element map: the contract is what `clickAt`
/// answers, and a test that resolved topology itself would be checking its own second copy of the
/// code under test.
std::vector<std::uint32_t> slotsAcceptedAt(Controller& app, cad::document::ObjectId id, Level level) {
    const Level restore = app.selectionLevel();
    app.setSelectionLevel(level);
    std::vector<std::uint32_t> slots;
    for (std::uint32_t slot = 0; slot < 128; ++slot) {
        app.scriptNextPick(slot);
        const auto pick = app.pickAt(10, 10);
        if (!pick.hit || pick.object != id) continue;

        app.scriptNextPick(slot);
        const auto click = app.clickAt(10, 10, /*additive=*/false);
        if (click.changed) slots.push_back(slot);
    }
    app.setSelectionLevel(restore);
    app.clearSelection();
    return slots;
}

}  // namespace

TEST_CASE("a click at Body level selects the feature it belongs to", "[selection]") {
    Controller app;
    const auto id = create(app, "feature.box");
    app.clearSelection();
    REQUIRE(app.selection().empty());

    app.scriptNextPick(0);
    const auto click = app.clickAt(10, 10, false);
    CHECK(click.hit);
    CHECK(click.changed);
    REQUIRE(app.selection().size() == 1);
    CHECK(app.selection().front() == id);
    // The message names the feature. At Body level "Selected" alone would be true and useless.
    CHECK(click.message.find("Box") != std::string::npos);
}

TEST_CASE("a click on empty space clears the selection, unless additive", "[selection]") {
    Controller app;
    create(app, "feature.box");
    REQUIRE(app.selection().size() == 1);

    app.scriptNextPick(0, /*valid=*/false);
    const auto additive = app.clickAt(10, 10, true);
    CHECK_FALSE(additive.hit);
    // Ctrl-clicking nothing is a miss, not "deselect everything". Clearing here would make an
    // imprecise multi-select click destroy the selection being built.
    CHECK(app.selection().size() == 1);

    app.scriptNextPick(0, /*valid=*/false);
    const auto plain = app.clickAt(10, 10, false);
    CHECK_FALSE(plain.hit);
    CHECK(plain.changed);
    CHECK(app.selection().empty());
}

TEST_CASE("clicking a selected thing again deselects it", "[selection]") {
    Controller app;
    const auto id = create(app, "feature.box");
    app.clearSelection();

    app.scriptNextPick(0);
    app.clickAt(10, 10, false);
    REQUIRE(app.selection().size() == 1);
    app.scriptNextPick(0);
    app.clickAt(10, 10, true);
    CHECK(app.selection().empty());
    (void)id;
}

TEST_CASE("Face level selects faces and refuses edges", "[selection]") {
    Controller app;
    const auto id = create(app, "feature.box");

    const auto faces = slotsAcceptedAt(app, id, Level::Face);
    const auto edges = slotsAcceptedAt(app, id, Level::Edge);

    // A box: six faces, twelve edges. Asserted exactly, because "some faces" would pass just as well
    // if the level were being ignored and every slot accepted.
    CHECK(faces.size() == 6);
    CHECK(edges.size() == 12);

    // No slot may be accepted at both levels: a thing is a face or an edge, never both.
    for (const std::uint32_t slot : faces) {
        CHECK(std::find(edges.begin(), edges.end(), slot) == edges.end());
    }
}

TEST_CASE("a refused click explains itself", "[selection]") {
    // Vertex level is the honest case: the mesh carries faces and edges, so there is nothing to hit
    // at all. Reporting that beats a click that silently does nothing, which is indistinguishable
    // from a broken picker -- the complaint this whole seam exists to answer.
    Controller app;
    const auto id = create(app, "feature.box");
    app.setSelectionLevel(Level::Vertex);

    bool sawRefusal = false;
    for (std::uint32_t slot = 0; slot < 64; ++slot) {
        app.scriptNextPick(slot);
        const auto pick = app.pickAt(10, 10);
        if (!pick.hit || pick.object != id) continue;

        app.scriptNextPick(slot);
        const auto click = app.clickAt(10, 10, false);
        CHECK_FALSE(click.changed);
        CHECK_FALSE(click.message.empty());
        sawRefusal = true;
    }
    CHECK(sawRefusal);
    CHECK(app.elementSelection().empty());
}

TEST_CASE("element selection accumulates and is reported to commands", "[selection]") {
    Controller app;
    const auto id = create(app, "feature.box");
    const auto edges = slotsAcceptedAt(app, id, Level::Edge);
    REQUIRE(edges.size() >= 3);

    app.setSelectionLevel(Level::Edge);
    for (std::size_t i = 0; i < 3; ++i) {
        app.scriptNextPick(edges[i]);
        app.clickAt(10, 10, /*additive=*/true);
    }
    REQUIRE(app.elementSelection().size() == 3);
    // The count commands gate on. It was hard-coded to 0 before element selection existed, so every
    // command that wanted geometry had to pretend it had none.
    CHECK(app.context().selectedElements == 3);

    // Every entry names a distinct element of the same object.
    std::vector<std::string> names;
    for (const auto& picked : app.elementSelection()) {
        CHECK(picked.object == id);
        names.push_back(picked.element.toString());
    }
    std::sort(names.begin(), names.end());
    CHECK(std::unique(names.begin(), names.end()) == names.end());
}

TEST_CASE("changing level drops the selection made at the old one", "[selection]") {
    // Not tidiness. There is no honest mapping from "this face" to "this edge", so keeping a face
    // selected while the level reads Edge would let a command act on something the user can no longer
    // see is selected.
    Controller app;
    const auto id = create(app, "feature.box");
    const auto faces = slotsAcceptedAt(app, id, Level::Face);
    REQUIRE_FALSE(faces.empty());

    app.setSelectionLevel(Level::Face);
    app.scriptNextPick(faces.front());
    app.clickAt(10, 10, false);
    REQUIRE(app.elementSelection().size() == 1);

    app.setSelectionLevel(Level::Edge);
    CHECK(app.elementSelection().empty());

    // Setting the same level twice is not a change and must not clear anything.
    app.setSelectionLevel(Level::Edge);
    const auto edges = slotsAcceptedAt(app, id, Level::Edge);
    REQUIRE_FALSE(edges.empty());
    app.setSelectionLevel(Level::Edge);
    app.scriptNextPick(edges.front());
    app.clickAt(10, 10, false);
    REQUIRE(app.elementSelection().size() == 1);
    app.setSelectionLevel(Level::Edge);
    CHECK(app.elementSelection().size() == 1);
}

TEST_CASE("a selected element does not outlive its feature", "[selection]") {
    // An undo or a delete leaves an element slot pointing at whatever now occupies it. Keeping the
    // selection would highlight unrelated geometry and hand a command a reference to a feature that
    // no longer exists.
    Controller app;
    const auto id = create(app, "feature.box");
    const auto faces = slotsAcceptedAt(app, id, Level::Face);
    REQUIRE_FALSE(faces.empty());

    app.setSelectionLevel(Level::Face);
    app.scriptNextPick(faces.front());
    app.clickAt(10, 10, false);
    REQUIRE(app.elementSelection().size() == 1);

    app.remove(id);
    app.refresh();
    CHECK(app.elementSelection().empty());
}

TEST_CASE("hover changes only when the element under the pointer changes", "[selection]") {
    // The shell repaints on this return value. Hover fires on every mouse-move, so a true on every
    // event is the difference between a responsive viewport and a warm laptop.
    Controller app;
    create(app, "feature.box");

    app.scriptNextPick(0);
    CHECK(app.hoverAt(10, 10));        // nothing -> slot 0
    app.scriptNextPick(0);
    CHECK_FALSE(app.hoverAt(11, 11));  // same element, moved a pixel
    app.scriptNextPick(1);
    CHECK(app.hoverAt(12, 12));        // a different element

    CHECK(app.clearHover());
    CHECK_FALSE(app.clearHover());     // already clear

    app.scriptNextPick(0, /*valid=*/false);
    CHECK_FALSE(app.hoverAt(13, 13));  // still nothing under the pointer
}

TEST_CASE("selection survives a recompute", "[selection]") {
    // A scene rebuild resizes the highlight table, which drops what was marked -- and every edit ends
    // in a recompute, so without re-pushing, the selection would visibly unhighlight itself most of
    // the time while still being selected.
    Controller app;
    const auto id = create(app, "feature.box");
    const auto faces = slotsAcceptedAt(app, id, Level::Face);
    REQUIRE_FALSE(faces.empty());

    app.setSelectionLevel(Level::Face);
    app.scriptNextPick(faces.front());
    app.clickAt(10, 10, false);
    const auto before = app.elementSelection();
    REQUIRE(before.size() == 1);

    app.refresh();
    REQUIRE(app.elementSelection().size() == 1);
    CHECK(app.elementSelection().front().element == before.front().element);
    CHECK(app.elementSelection().front().slot == before.front().slot);
}

TEST_CASE("a fillet takes the edges that were picked", "[selection][feature]") {
    // What edge selection was FOR. Before it existed, addEdgeFeature called edgesOf() and filleted
    // every edge of the body, and the status line admitted as much. The count is the assertion: a
    // fillet built from three picked edges must carry three, not twelve.
    Controller app;
    const auto id = create(app, "feature.box");
    const auto edges = slotsAcceptedAt(app, id, Level::Edge);
    REQUIRE(edges.size() == 12);

    app.setSelectionLevel(Level::Edge);
    for (std::size_t i = 0; i < 3; ++i) {
        app.scriptNextPick(edges[i]);
        app.clickAt(10, 10, /*additive=*/true);
    }
    REQUIRE(app.elementSelection().size() == 3);

    std::string lastStatus;
    app.onStatus([&lastStatus](const std::string& s) { lastStatus = s; });

    bool invoked = false;
    for (const auto& command : app.commands()) {
        if (command.id != "feature.fillet") continue;
        // The command must OFFER itself on an edge selection. Greyed out with three edges picked
        // would read as the selection not counting for anything.
        CHECK(command.enabled(app.context()));
        command.invoke();
        invoked = true;
        break;
    }
    REQUIRE(invoked);
    app.refresh();

    INFO("status: " << lastStatus);
    CHECK(lastStatus.find("3") != std::string::npos);
    CHECK(lastStatus.find("all") == std::string::npos);

    // The picked edges belonged to the old feature's output, which the fillet consumed. Leaving them
    // selected would mark geometry that no longer exists.
    CHECK(app.elementSelection().empty());
}

TEST_CASE("a whole-body fillet still works, and says so", "[selection][feature]") {
    // The old path is not removed, it is the fallback: selecting a body and filleting it is a real
    // thing to want. It just has to be honest that it took every edge.
    Controller app;
    create(app, "feature.box");
    std::string lastStatus;
    app.onStatus([&lastStatus](const std::string& s) { lastStatus = s; });

    for (const auto& command : app.commands()) {
        if (command.id != "feature.fillet") continue;
        REQUIRE(command.enabled(app.context()));
        command.invoke();
        break;
    }
    app.refresh();
    INFO("status: " << lastStatus);
    CHECK(lastStatus.find("12") != std::string::npos);
}

TEST_CASE("edges from two bodies are refused rather than half-used", "[selection][feature]") {
    // A fillet takes a base shape and edges OF it. Edges from two bodies is not one feature with an
    // odd input; it is two features, and picking one silently discards half of what was selected.
    Controller app;
    const auto box = create(app, "feature.box");
    const auto cyl = create(app, "feature.cylinder");

    const auto boxEdges = slotsAcceptedAt(app, box, Level::Edge);
    const auto cylEdges = slotsAcceptedAt(app, cyl, Level::Edge);
    REQUIRE_FALSE(boxEdges.empty());
    REQUIRE_FALSE(cylEdges.empty());

    app.setSelectionLevel(Level::Edge);
    app.scriptNextPick(boxEdges.front());
    app.clickAt(10, 10, true);
    app.scriptNextPick(cylEdges.front());
    app.clickAt(10, 10, true);
    REQUIRE(app.elementSelection().size() == 2);

    std::string lastStatus;
    app.onStatus([&lastStatus](const std::string& s) { lastStatus = s; });
    const std::size_t before = app.tree().size();

    for (const auto& command : app.commands()) {
        if (command.id != "feature.fillet") continue;
        command.invoke();
        break;
    }
    INFO("status: " << lastStatus);
    CHECK(lastStatus.find("one body") != std::string::npos);
    CHECK(app.tree().size() == before);   // nothing was added
}

TEST_CASE("a selection marks the scene's highlight table", "[selection][render]") {
    // The step between "selected" and "visible". The highlight table is what the renderer reads, and
    // for a long time the backend ignored it entirely -- so a selection that never reaches this table
    // and a table the shader never samples look identical from the outside: nothing turns blue.
    Controller app;
    const auto id = create(app, "feature.box");

    const auto marked = [&app] {
        std::size_t n = 0;
        for (const auto h : app.frame().highlights) {
            if (h != cad::render::Highlight::None) ++n;
        }
        return n;
    };

    app.clearSelection();
    REQUIRE(app.frame().elementCount > 0);
    CHECK(marked() == 0);

    // A whole body marks every element it owns -- 18 for a box, 6 faces and 12 edges.
    app.setSelection({id});
    CHECK(marked() == 18);

    app.clearSelection();
    CHECK(marked() == 0);

    // One face marks exactly one.
    const auto faces = slotsAcceptedAt(app, id, Level::Face);
    REQUIRE_FALSE(faces.empty());
    app.setSelectionLevel(Level::Face);
    app.scriptNextPick(faces.front());
    app.clickAt(10, 10, false);
    CHECK(marked() == 1);

    // Hover adds a second mark without replacing the selection.
    app.scriptNextPick(faces.back());
    app.hoverAt(20, 20);
    CHECK(marked() == 2);
}
