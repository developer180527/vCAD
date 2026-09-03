// A selection stays visible after the pointer moves away.
//
// # What was wrong
//
// Reported from the UI: double clicking a box turned it completely blue, and moving the cursor off
// it turned the highlight off again — "sign that nothing is selected". The Controller had the body
// selected the whole time. Only the paint went missing.
//
// `refreshHighlights` chose what to draw from the selection LEVEL:
//
//     if (selectionLevel_ == Body) { draw the selected bodies }
//     else                         { draw the selected elements }
//
// A double click selects a body by swapping the level to Body for the duration of one `tapAt` and
// then putting it back. So the highlight painted during that call — whole body, blue — and the next
// refresh, which a mouse move triggers, ran at Auto, took the element branch, found no elements
// selected and painted nothing.
//
// # Why this is the same bug the commands had
//
// `Commands.cpp` says it outright: "By what is SELECTED, never by the selection level. The level
// says what a click resolves to; it does not describe what is already selected." That rule was
// learned twice for command enablement — it is why Hole and Revolve were once unreachable — and
// the highlight path never got it.
//
// It is also why the iPad was fine on identical shared code: `selectionLevel_` defaults to Body and
// the iPad leaves it there, while the desktop sets Auto.
//
// # What is asserted
//
// That the highlight survives the level going back to Auto. Measured through the edge overlay
// batches, which is the same proxy edge_highlight.cpp uses: highlighting adds draw calls, so their
// disappearance is the bug made countable.

#include "cad/app/Controller.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

using cad::app::Controller;
using Level = Controller::SelectionLevel;

namespace {

const cad::app::Command* commandNamed(const Controller& app, const std::string& id) {
    for (const auto& command : app.commands()) {
        if (command.id == id) return &command;
    }
    return nullptr;
}

cad::document::ObjectId aBox(Controller& app) {
    commandNamed(app, "feature.box")->invoke();
    app.refresh();
    REQUIRE(app.selection().size() == 1);
    return app.selection().front();
}

/// How many draw calls the frame is spending on highlights.
std::size_t highlightBatches(const Controller& app) {
    return app.frame().edgeBatches.size();
}

}  // namespace

TEST_CASE("a selected body stays highlighted when the level is not Body", "[selection][highlight]") {
    // The regression itself. A double click leaves the body selected and the level back at Auto,
    // and that combination drew nothing at all.
    Controller app;
    const auto box = aBox(app);

    app.setSelectionLevel(Level::Body);
    app.clearSelection();
    const std::size_t unselected = highlightBatches(app);

    app.select(box, /*additive=*/false);
    const std::size_t selectedAtBody = highlightBatches(app);
    INFO("batches: " << unselected << " unselected, " << selectedAtBody << " selected at Body");
    REQUIRE(selectedAtBody > unselected);   // the proxy works at all

    // The level goes back to what the desktop actually uses. The body is still selected — nothing
    // below has been asked to change — so the highlight must still be there.
    app.setSelectionLevel(Level::Auto);
    INFO("after returning to Auto: " << highlightBatches(app));
    CHECK(highlightBatches(app) == selectedAtBody);

    // And the model agrees it is still selected, which is what made this so confusing to report:
    // every command needing a body stayed enabled while nothing looked chosen.
    CHECK(app.selection().size() == 1);
}

TEST_CASE("clearing the selection does remove the highlight", "[selection][highlight]") {
    // The control. A "fix" that simply drew every body always would pass the test above and make
    // the highlight meaningless — which is the more embarrassing bug of the two, because it looks
    // like everything is selected rather than nothing.
    Controller app;
    const auto box = aBox(app);

    app.setSelectionLevel(Level::Auto);
    app.clearSelection();
    const std::size_t unselected = highlightBatches(app);

    app.select(box, /*additive=*/false);
    REQUIRE(highlightBatches(app) > unselected);

    app.clearSelection();
    CHECK(highlightBatches(app) == unselected);
}

TEST_CASE("a body selected at Auto is highlighted at once", "[selection][highlight]") {
    // Selecting from the model tree does not touch the level, and the desktop sits at Auto, so this
    // is the path a user takes far more often than the double click that exposed the bug. It was
    // broken the whole time and read as "the browser selects but the viewport does not show it".
    Controller app;
    const auto box = aBox(app);
    app.setSelectionLevel(Level::Auto);
    app.clearSelection();
    const std::size_t unselected = highlightBatches(app);

    app.select(box, /*additive=*/false);
    CHECK(highlightBatches(app) > unselected);
}
