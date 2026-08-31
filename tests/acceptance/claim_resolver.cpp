/// Resolving claims: the decision that turns "which name should this face carry" into a map.
///
/// # Why this test file exists at all
///
/// Everything this exercises used to be reachable only by building real geometry and running a real
/// boolean. That is not a coverage complaint -- it had a concrete cost. A mutation that deleted the
/// canonical ordering of split siblings ENTIRELY passed all 742 naming assertions, because within
/// one process OCCT's traversal order is stable enough to hide it. An alias test written the same
/// way turned out to be asserting a property of an empty set, and had to be deleted.
///
/// The rules below are decisions about names and shapes, not about geometry. They are stated here
/// directly, against claims built by hand, so that each one can be broken on purpose and seen to
/// fail. `m1_naming_stability.cpp` still checks that real booleans reach this code correctly; that
/// is a different question and it needs the geometry.
///
/// Reached by relative path into core/naming/src, like the plugin manifest parser: this is an
/// internal decision of the naming layer, and giving it a public header would invite a feature to
/// depend on how names are assigned rather than on what they mean.

#include "../../core/naming/src/ClaimResolver.h"
#include "../../core/naming/src/Measure.h"

#include "cad/kernel/Primitives.h"
#include "cad/kernel/Transform.h"
#include "cad/kernel/internal/Occt.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace cad;
using naming::ElementName;
using naming::NameStep;
using naming::Provenance;
using naming::internal::Claim;
using naming::internal::resolveClaims;

namespace {

/// A distinct, deterministic name. The content does not matter to the resolver -- only whether two
/// names are equal -- so these are minimal.
ElementName named(std::uint32_t serial, std::uint32_t discriminator = 0) {
    NameStep step;
    step.featureSerial = serial;
    step.opTag = 0;
    step.provenance = Provenance::Primitive;
    step.discriminator = discriminator;
    return ElementName({step});
}

/// The faces of a box, as raw shapes to claim.
std::vector<kernel::Shape> facesOf(double dx, double dy, double dz, double atX = 0.0) {
    auto made = kernel::makeBox(dx, dy, dz);
    REQUIRE(made.ok());
    kernel::Shape shape = made.value().op.shape();
    if (atX != 0.0) {
        auto moved = kernel::translate(shape, atX, 0.0, 0.0);
        REQUIRE(moved.ok());
        shape = moved.value().shape();
    }
    auto faces = shape.subShapes(kernel::ShapeType::Face);
    REQUIRE(faces.size() >= 6);
    return faces;
}

/// The raw OCCT shape behind one of our wrappers.
const TopoDS_Shape& occtOf(const kernel::Shape& s) {
    return kernel::occt(const_cast<kernel::Shape&>(s));
}

}   // namespace

TEST_CASE("a face claimed once keeps exactly the name it was claimed under",
          "[naming][claims]") {
    // The common case, and the compatibility guarantee: an unsplit face must not acquire a derived
    // step just because splitting exists. Every reference in every saved document depends on it.
    const auto faces = facesOf(40, 30, 20);
    const auto name = named(1, 4);

    const auto map = resolveClaims({Claim{occtOf(faces[0]), name, occtOf(faces[0])}},
                                   3, 0);

    CHECK(map.size() == 1);
    CHECK(map.collisions().empty());
    REQUIRE(map.nameOf(faces[0]).has_value());
    CHECK(*map.nameOf(faces[0]) == name);   // identical, not derived
}

TEST_CASE("two faces from one parent are the pieces of a split", "[naming][claims]") {
    // What a boolean does when it cuts a face in half: both pieces are reported as the same parent,
    // modified. They are different elements and must end up with different names.
    const auto faces = facesOf(40, 30, 20);
    const auto name = named(1, 4);
    const auto parent = occtOf(faces[0]);

    const auto map = resolveClaims({Claim{occtOf(faces[1]), name, parent},
                                    Claim{occtOf(faces[2]), name, parent}},
                                   3, 0);

    CHECK(map.collisions().empty());
    const auto first = map.nameOf(faces[1]);
    const auto second = map.nameOf(faces[2]);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK_FALSE(*first == *second);
    // Both still trace back to the face they are pieces of, which is what makes "the top face"
    // answerable after it splits.
    CHECK(first->family() == second->family());

    // And WHICH piece gets which number is decided by the geometry, not by the order the claims
    // happened to arrive in. Numbering them by arrival would be an index-based identity wearing a
    // derivation chain's clothes: rebuild the same model with the operands the other way round and
    // every reference into the split would move to the other half.
    const auto swapped = resolveClaims({Claim{occtOf(faces[2]), name, parent},
                                        Claim{occtOf(faces[1]), name, parent}},
                                       3, 0);
    REQUIRE(swapped.nameOf(faces[1]).has_value());
    REQUIRE(swapped.nameOf(faces[2]).has_value());
    CHECK(*swapped.nameOf(faces[1]) == *first);
    CHECK(*swapped.nameOf(faces[2]) == *second);
}

TEST_CASE("two faces from DIFFERENT parents stay a collision", "[naming][claims]") {
    // The case that must NOT be silently numbered. Two input faces carrying the same name are two
    // elements the naming layer cannot tell apart -- what an unstamped copy produces -- and
    // assigning a user's reference to one of them by area order would be a guess presented as a
    // fact. It has to remain a collision so the caller raises NamingLost.
    const auto left = facesOf(40, 30, 20);
    const auto right = facesOf(40, 30, 20, 200.0);
    const auto shared = named(1, 4);

    const auto map = resolveClaims({Claim{occtOf(left[1]), shared, occtOf(left[0])},
                                    Claim{occtOf(right[1]), shared, occtOf(right[0])}},
                                   3, 0);

    CHECK_FALSE(map.collisions().empty());
}

TEST_CASE("two names on one face are a merge, and the smaller one is canonical",
          "[naming][claims]") {
    // Two coplanar faces unified into one. Both references must survive, so both names are bound;
    // the lexicographically smallest is canonical so that the choice does not depend on the order
    // the claims happened to arrive in.
    const auto faces = facesOf(40, 30, 20);
    const auto smaller = named(1, 1);
    const auto larger = named(1, 9);
    const auto face = occtOf(faces[0]);

    for (const bool largerFirst : {false, true}) {
        std::vector<Claim> claims;
        if (largerFirst) {
            claims = {Claim{face, larger, face}, Claim{face, smaller, face}};
        } else {
            claims = {Claim{face, smaller, face}, Claim{face, larger, face}};
        }
        const auto map = resolveClaims(claims, 3, 0);

        INFO("larger claimed first: " << largerFirst);
        // Both resolve: a reference to either survives the merge.
        CHECK(map.resolve(smaller).has_value());
        CHECK(map.resolve(larger).has_value());
        // And the element's own name is the canonical one either way. This is what every edge and
        // vertex is derived from, so an answer that depended on claim order would make the whole
        // shape's naming depend on the order its inputs were passed.
        REQUIRE(map.nameOf(faces[0]).has_value());
        CHECK(*map.nameOf(faces[0]) == smaller);
    }
}

TEST_CASE("siblings that cannot be told apart are refused, not numbered", "[naming][claims]") {
    // Two faces with identical area and centroid. std::sort leaves equal elements in an unspecified
    // order and the sibling index comes straight from that order, so numbering them would hand a
    // reference to one of two indistinguishable pieces by whatever the standard library did that
    // day -- silently, and differently on another machine.
    //
    // Two separately built boxes at the same place give exactly that: distinct shapes, identical
    // measurements.
    const auto first = facesOf(40, 30, 20);
    const auto second = facesOf(40, 30, 20);
    REQUIRE(naming::internal::measureFace(occtOf(first[0]))
            == naming::internal::measureFace(occtOf(second[0])));

    const auto name = named(1, 4);
    const auto parent = occtOf(first[1]);
    const auto map = resolveClaims({Claim{occtOf(first[0]), name, parent},
                                    Claim{occtOf(second[0]), name, parent}},
                                   3, 0);

    // Left colliding on purpose: the caller turns this into NamingLost. ADR 0005 is explicit that
    // a wrong reference is worse than a failed one.
    CHECK_FALSE(map.collisions().empty());
}

TEST_CASE("the same claim made twice is not a merge", "[naming][claims]") {
    // OCCT can report the same correspondence through more than one route -- Modified and the
    // geometric fallback both reaching the same conclusion. Treating the repeat as a second
    // claimant would alias a name to itself and inflate the map.
    const auto faces = facesOf(40, 30, 20);
    const auto name = named(1, 4);
    const auto face = occtOf(faces[0]);

    const auto map = resolveClaims({Claim{face, name, face}, Claim{face, name, face}}, 3, 0);
    CHECK(map.size() == 1);
    CHECK(map.collisions().empty());
}

TEST_CASE("resolving no claims produces an empty map rather than misbehaving",
          "[naming][claims]") {
    const auto map = resolveClaims({}, 3, 0);
    CHECK(map.size() == 0);
    CHECK(map.collisions().empty());
}

// ── canonical ordering ─────────────────────────────────────────────────────────────────
//
// The one policy that five places in the naming layer used to write out longhand, four of them
// missing the same check.

TEST_CASE("ordering is decided by the measurement, not by input order", "[naming][ordering]") {
    using naming::internal::canonicalOrder;
    using naming::internal::measureFace;

    const auto faces = facesOf(40, 30, 20);
    std::vector<TopoDS_Shape> forward{occtOf(faces[0]), occtOf(faces[1]), occtOf(faces[2])};
    std::vector<TopoDS_Shape> backward{occtOf(faces[2]), occtOf(faces[1]), occtOf(faces[0])};

    const auto a = canonicalOrder(forward, measureFace);
    const auto b = canonicalOrder(backward, measureFace);
    REQUIRE_FALSE(a.ambiguous);
    REQUIRE(a.elements.size() == 3);
    for (std::size_t i = 0; i < a.elements.size(); ++i) {
        CHECK(a.elements[i].IsSame(b.elements[i]));
    }
}

TEST_CASE("a measurement that does not separate siblings orders nothing", "[naming][ordering]") {
    // This is not hypothetical -- it is the bug this helper was extracted to expose.
    //
    // Two places named an open sketch's edges and vertices by sorting them with `measureFace`,
    // which reads SURFACE properties: zero area and the origin for every edge and every vertex. So
    // every element measured identically, the sort did nothing, and the persistent discriminator
    // was the element's index in OCCT's traversal order. Reorder the sketch and every reference
    // into it moves -- which is precisely the index-based identity ADR 0005 exists to abolish, and
    // a revolve names its axis this way.
    using naming::internal::canonicalOrder;
    using naming::internal::measureFace;
    using naming::internal::midpointOf;

    auto made = kernel::makeBox(40, 30, 20);
    REQUIRE(made.ok());
    const auto shape = made.value().op.shape();
    const auto edges = shape.subShapes(kernel::ShapeType::Edge);
    REQUIRE(edges.size() >= 3);

    std::vector<TopoDS_Shape> raw;
    for (std::size_t i = 0; i < 3; ++i) raw.push_back(occtOf(edges[i]));

    // The wrong measure: every edge measures the same, so nothing can be numbered.
    const auto bySurface = canonicalOrder(raw, measureFace);
    CHECK(bySurface.ambiguous);
    CHECK(bySurface.elements.empty());

    // The right one separates them, which is what makes the names mean anything.
    const auto byMidpoint = canonicalOrder(raw, midpointOf);
    CHECK_FALSE(byMidpoint.ambiguous);
    CHECK(byMidpoint.elements.size() == 3);
}

TEST_CASE("a single sibling needs no ordering and is never ambiguous", "[naming][ordering]") {
    const auto faces = facesOf(40, 30, 20);
    const auto one = naming::internal::canonicalOrder({occtOf(faces[0])},
                                                      naming::internal::measureFace);
    CHECK_FALSE(one.ambiguous);
    CHECK(one.elements.size() == 1);

    const auto none = naming::internal::canonicalOrder({}, naming::internal::measureFace);
    CHECK_FALSE(none.ambiguous);
    CHECK(none.elements.empty());
}
