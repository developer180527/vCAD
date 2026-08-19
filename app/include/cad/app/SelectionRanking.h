#pragma once

/// Which of several things under the pointer the user meant.
///
/// # Why this is its own type, with no dependencies
///
/// A tap covers an area — 88 device pixels across on a Retina iPad, because that is a fingertip —
/// and inside that area there are usually several elements. Choosing between them is a RULE about
/// what pointing means, so it belongs above the renderer and below the shell: the iPad and the
/// desktop must not answer it differently.
///
/// It takes candidates and returns an order. No GPU, no document, no camera — which is what makes
/// the rule testable at all. The interesting cases (an edge one pixel inside a face, two edges a
/// pixel apart, a vertex on the silhouette) are miserable to arrange in front of a real rasteriser
/// and trivial to write down as three structs.
///
/// # The rule, and why it is not "nearest depth"
///
/// Depth alone is the obvious answer and the wrong one. The frontmost thing under a fingertip is
/// almost always a FACE, because faces cover area and edges do not — rank by depth and edges become
/// unselectable by touch, on a tablet where edge selection matters most.
///
///   1. **Kind**: vertex, then edge, then face. Small targets beat large ones, which is what makes
///      "tap near an edge" select the edge rather than the face behind it.
///   2. **Screen distance** from the tap centre, so between two edges the nearer wins.
///   3. **Depth**, which only separates candidates that are equally near and the same kind.
///
/// See docs/design/SELECTION.md for the vendor behaviour this matches and the sources.

#include <cstdint>
#include <vector>

namespace cad::app {

/// What kind of thing a candidate is. Ordered by how hard it is to hit deliberately, which is the
/// order the ranking uses — the enum's values ARE the priority, smallest first.
enum class PickKind : std::uint8_t { Vertex = 0, Edge = 1, Face = 2, Unknown = 3 };

/// One thing found inside the aperture.
struct PickCandidate {
    std::uint32_t slot = 0;   ///< absolute element slot, as the id buffer encodes it
    PickKind kind = PickKind::Unknown;
    /// Distance from the tap centre in device pixels, squared. Squared because nothing here needs
    /// the actual distance and a square root per candidate buys only rounding error.
    std::uint64_t distanceSq = 0;
    float depth = 1.0f;
};

/// Orders candidates best-first, in place.
///
/// Stable, so two candidates the rule cannot separate stay in the order the picker found them —
/// which makes a Select Other list deterministic rather than shuffling between identical taps.
void rankCandidates(std::vector<PickCandidate>&);

/// Drops candidates that the active selection level cannot resolve to.
///
/// A level RESTRICTS rather than reorders: it decides what a pick resolves to, so at Edge level a
/// face under the finger is not a worse candidate, it is not a candidate. Body level keeps
/// everything, because a body is reached through whichever element was hit.
void restrictToKind(std::vector<PickCandidate>&, PickKind);

}   // namespace cad::app
