/// Tap-to-select: what a pointing gesture that covers an AREA resolves to.
///
/// # Why these tests are at this level
///
/// The rule under test is "which of the things under the finger did the user mean", and the cases
/// that matter are all near-misses: an edge one pixel inside a face, two edges a pixel apart, a
/// face that is nearer in depth than the edge crossing it. Arranging those in front of a real
/// rasteriser means positioning a camera so that specific geometry lands on specific pixels — a
/// test that would be measuring the camera, and would break for reasons having nothing to do with
/// selection.
///
/// So the ranking is a pure function over candidates, and these tests state the cases directly.
/// What a GPU actually writes into the id buffer is a different question, answered by the pick
/// tests that need one.
///
/// # The failure this is really guarding
///
/// Ranking by depth is the obvious implementation and it is wrong: the frontmost thing under a
/// fingertip is nearly always a FACE, because faces cover area and edges do not. Depth-first
/// ranking compiles, runs, passes any test that only checks "something was selected", and makes
/// edge selection impossible on a tablet. Three of the cases below fail under it.

#include "cad/app/Controller.h"
#include "cad/app/SelectionRanking.h"

#include <catch2/catch_test_macros.hpp>

using namespace cad;
using namespace cad::app;

namespace {

PickCandidate at(std::uint32_t slot, PickKind kind, std::uint64_t distanceSq, float depth = 0.5f) {
    return PickCandidate{slot, kind, distanceSq, depth};
}

}   // namespace

TEST_CASE("a small target beats a large one under the same finger", "[selection]") {
    // The case that makes touch selection work at all: the user aimed at an edge, and the face it
    // borders is both nearer to the tap centre AND in front of it.
    std::vector<PickCandidate> candidates{
        at(1, PickKind::Face, 0, 0.10f),
        at(2, PickKind::Edge, 25, 0.50f),
    };
    rankCandidates(candidates);
    CHECK(candidates.front().slot == 2);
}

TEST_CASE("a vertex beats an edge beats a face", "[selection]") {
    std::vector<PickCandidate> candidates{
        at(1, PickKind::Face, 0),
        at(2, PickKind::Edge, 0),
        at(3, PickKind::Vertex, 0),
    };
    rankCandidates(candidates);
    REQUIRE(candidates.size() == 3);
    CHECK(candidates[0].slot == 3);
    CHECK(candidates[1].slot == 2);
    CHECK(candidates[2].slot == 1);
}

TEST_CASE("between two of the same kind, the nearer to the tap wins", "[selection]") {
    std::vector<PickCandidate> candidates{
        at(1, PickKind::Edge, 100, 0.10f),
        at(2, PickKind::Edge, 4, 0.90f),   // further back, much closer to the finger
    };
    rankCandidates(candidates);
    CHECK(candidates.front().slot == 2);
}

TEST_CASE("depth separates candidates that are otherwise identical", "[selection]") {
    std::vector<PickCandidate> candidates{
        at(1, PickKind::Face, 9, 0.80f),
        at(2, PickKind::Face, 9, 0.20f),
    };
    rankCandidates(candidates);
    CHECK(candidates.front().slot == 2);
}

TEST_CASE("the order of equal candidates is stable", "[selection]") {
    // Not a nicety. A Select Other list that reorders itself between two identical taps is a list
    // the user cannot point at, and a flat face produces dozens of exactly-equal candidates.
    std::vector<PickCandidate> candidates{
        at(7, PickKind::Face, 4, 0.5f),
        at(3, PickKind::Face, 4, 0.5f),
        at(9, PickKind::Face, 4, 0.5f),
    };
    rankCandidates(candidates);
    CHECK(candidates[0].slot == 7);
    CHECK(candidates[1].slot == 3);
    CHECK(candidates[2].slot == 9);
}

TEST_CASE("a selection level removes candidates rather than demoting them", "[selection]") {
    std::vector<PickCandidate> candidates{
        at(1, PickKind::Face, 0),
        at(2, PickKind::Edge, 25),
    };
    restrictToKind(candidates, PickKind::Face);
    REQUIRE(candidates.size() == 1);
    CHECK(candidates.front().slot == 1);
}

TEST_CASE("an empty aperture ranks to nothing rather than misbehaving", "[selection]") {
    std::vector<PickCandidate> candidates;
    rankCandidates(candidates);
    CHECK(candidates.empty());
    restrictToKind(candidates, PickKind::Edge);
    CHECK(candidates.empty());
}

TEST_CASE("a tap with no aperture hit clears the selection like a click", "[selection]") {
    // The shared-path guarantee, exercised through the real Controller against the null backend:
    // tapAt and clickAt must agree about everything except how they find the thing.
    Controller controller;
    // beginCommand OPENS a command's parameter panel; commitCommand is what applies it. Adding the
    // primitive selects it, so the selection is non-empty afterwards. Not asserted through document
    // size: a new document also carries the three origin datums, which are objects too.
    REQUIRE(controller.beginCommand("feature.box"));
    REQUIRE(controller.commitCommand());
    REQUIRE_FALSE(controller.selection().empty());

    // The null picker reports nothing under the aperture unless a test scripts one.
    const auto result = controller.tapAt(10, 10, 44, false);
    CHECK_FALSE(result.hit);
    CHECK(controller.selection().empty());
}
