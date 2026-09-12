// An id an undo released is not free.
//
// # What was wrong
//
// `nextId` lives inside a Document, and undo restores an older Document -- so it handed the
// allocator back ids that were already spoken for. Found by the Rust suite's sequence property
// test, which shrank it to five actions: add a cylinder, undo three times, add a Translate
// referencing that cylinder.
//
// The Translate was then handed the cylinder's id, so it referenced ITSELF. Recompute reported
// "These features depend on each other in a loop: Translate"; the save SUCCEEDED; and the file
// could not be reopened, because opening recomputes and refuses a document that cannot rebuild.
// A file written and then unreadable is the worst shape a persistence bug can take.
//
// `Document::withNextId` already existed for this exact invariant, and says so: "Only ever forward.
// A loader that passed a stale value must not be able to walk the allocator backwards into ids that
// are already in use." Undo was doing precisely what the loader is forbidden to do.
//
// # Why it is worse than a dependency cycle
//
// An ObjectId is also the feature's naming serial, stamped into every element name it produces. Two
// features sharing one id name their faces identically, which is the collision the whole naming
// layer exists to make impossible -- see cache_key_identity.cpp for what that costs.
//
// # What is asserted
//
// That an id is never handed out twice, at the level where the allocator lives, plus the dependency
// consequence through the History API. The proptest that found it covers the save-and-reopen end of
// the story and cannot be relied on to run here: it searches randomly, and it found this on some
// runs and not others.

#include "cad/document/Document.h"

#include <catch2/catch_test_macros.hpp>

#include <variant>

using cad::document::Document;
using cad::document::History;

TEST_CASE("an id released by undo is never reissued", "[document][undo][id]") {
    History history(Document{});

    auto [withCylinder, cylinder] = history.current().add("Cylinder");
    history.commit(std::move(withCylinder), "Add Cylinder");

    REQUIRE(history.undo());

    // The same allocator, after going back. It must not offer the id it has already spent.
    auto [afterUndo, next] = history.current().add("Translate");
    INFO("cylinder was id " << cylinder.value << ", the next object got " << next.value);
    CHECK(next.value != cylinder.value);
}

TEST_CASE("a feature added after undo cannot reference itself", "[document][undo][id]") {
    // The symptom as reported, one layer up: with the id reused, a feature built to reference the
    // undone object referenced ITSELF, and the cycle only surfaced at recompute.
    History history(Document{});
    auto [withCylinder, cylinder] = history.current().add("Cylinder");
    history.commit(std::move(withCylinder), "Add Cylinder");
    REQUIRE(history.undo());

    auto [withTranslate, translate] = history.current().add("Translate");
    auto seeded = withTranslate.find(translate)->withProperty("a_base", cylinder);
    CHECK(translate.value != cylinder.value);

    // And the reference does not resolve to the new feature, which is what "references itself"
    // means in terms a reader can check.
    const auto* base = seeded.find("a_base");
    REQUIRE(base != nullptr);
    const auto* referenced = std::get_if<cad::document::ObjectId>(base);
    REQUIRE(referenced != nullptr);
    CHECK(referenced->value != translate.value);
    CHECK(referenced->value == cylinder.value);
}

TEST_CASE("redo does not walk the allocator backwards either", "[document][undo][id]") {
    // The other direction, because the fix touches both and a one-sided fix would leave the same
    // bug reachable through redo-then-edit.
    History history(Document{});
    auto [one, first] = history.current().add("Box");
    history.commit(std::move(one), "Add Box");
    REQUIRE(history.undo());
    REQUIRE(history.redo());

    auto [two, second] = history.current().add("Box");
    CHECK(second.value != first.value);
}
