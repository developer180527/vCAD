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

#include "cad/app/Controller.h"
#include "cad/io/Format.h"
#include "cad/kernel/Primitives.h"
#include "cad/kernel/Transform.h"
#include "cad/kernel/internal/Occt.h"

#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Ax2.hxx>
#include <gp_Circ.hxx>
#include <gp_Dir.hxx>
#include <gp_Pln.hxx>
#include <BRep_Builder.hxx>
#include <TopoDS_Edge.hxx>
#include <gp_Pnt.hxx>
#include <TopoDS_Compound.hxx>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
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
    REQUIRE_FALSE(a.anyAmbiguous());
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
    CHECK(bySurface.anyAmbiguous());
    // Every edge ties with its neighbours, so every one of them is marked.
    CHECK(bySurface.elements.size() == 3);
    for (const auto flag : bySurface.ambiguous) CHECK(flag == 1);

    // The right one separates them, which is what makes the names mean anything.
    const auto byMidpoint = canonicalOrder(raw, midpointOf);
    CHECK_FALSE(byMidpoint.anyAmbiguous());
    CHECK(byMidpoint.elements.size() == 3);
}

TEST_CASE("one tied pair does not cost the others their names", "[naming][ordering]") {
    // The reason ties are marked per element rather than per call. A supplier's STEP file with two
    // duplicate faces in it -- junk the exporter left behind -- must not leave every OTHER face of
    // the part unnamed, because that turns a defect in geometry the user will never touch into a
    // file that refuses to open.
    using naming::internal::canonicalOrder;
    using naming::internal::measureFace;

    const auto part = facesOf(40, 30, 20);
    const auto duplicate = facesOf(40, 30, 20);   // identical, so its faces tie with the first's

    std::vector<TopoDS_Shape> mixed{occtOf(part[0]), occtOf(part[1]), occtOf(duplicate[0])};
    const auto ordered = canonicalOrder(mixed, measureFace);

    REQUIRE(ordered.elements.size() == 3);
    CHECK(ordered.anyAmbiguous());
    std::size_t nameable = 0;
    for (const auto flag : ordered.ambiguous) {
        if (flag == 0) ++nameable;
    }
    // The tied pair loses its names; the third face keeps its own.
    CHECK(nameable == 1);
}

TEST_CASE("a single sibling needs no ordering and is never ambiguous", "[naming][ordering]") {
    const auto faces = facesOf(40, 30, 20);
    const auto one = naming::internal::canonicalOrder({occtOf(faces[0])},
                                                      naming::internal::measureFace);
    CHECK_FALSE(one.anyAmbiguous());
    CHECK(one.elements.size() == 1);

    const auto none = naming::internal::canonicalOrder({}, naming::internal::measureFace);
    CHECK_FALSE(none.anyAmbiguous());
    CHECK(none.elements.empty());
}

// ── naming geometry we read, rather than geometry we built ─────────────────────────────

TEST_CASE("a shape with indistinguishable faces can still be named best-effort",
          "[naming][import]") {
    // Two coincident copies of the same box, as a compound. Every face of one sits exactly on a
    // face of the other: same area, same centroid, genuinely indistinguishable by measurement.
    //
    // This is not a contrived shape. Supplier STEP files routinely carry duplicated or coincident
    // faces left behind by whatever exported them, and a user opening one wants to look at the
    // part, measure it, and export it onward.
    auto first = kernel::makeBox(40, 30, 20);
    auto second = kernel::makeBox(40, 30, 20);
    REQUIRE(first.ok());
    REQUIRE(second.ok());

    TopoDS_Compound compound;
    BRep_Builder builder;
    builder.MakeCompound(compound);
    builder.Add(compound, occtOf(first.value().op.shape()));
    builder.Add(compound, occtOf(second.value().op.shape()));
    const kernel::Shape doubled = kernel::wrap(compound);
    REQUIRE(doubled.subShapes(kernel::ShapeType::Face).size() == 12);

    // STRICT refuses: we could not identify everything, and for geometry we built that is a fault.
    {
        naming::NamingContext ctx(1, 0);
        const auto strict = ctx.nameprimitive(doubled, {});
        CHECK_FALSE(strict.ok());
    }

    // BEST EFFORT opens it. The faces that cannot be told apart are simply absent -- not guessed
    // at -- so a reference to one of them fails later, at the moment it is tried, rather than the
    // whole file being refused now.
    naming::NamingContext ctx(1, 0);
    const auto relaxed =
        ctx.nameprimitive(doubled, {}, naming::NamingContext::Naming::BestEffort);
    REQUIRE(relaxed.ok());

    // Nothing was invented: no two elements share a name.
    CHECK(relaxed.value().collisions().empty());
    // And the ambiguous ones really are absent rather than silently numbered.
    CHECK_FALSE(relaxed.value().unnamed(doubled).empty());
}

TEST_CASE("an imported file with unnameable faces still opens", "[naming][import]") {
    // End to end, through the Import FEATURE rather than through the naming layer directly,
    // because the decision under test is which mode Import asks for -- and asking for the strict
    // one is what used to turn a defect in a supplier's export into a file that would not open.
    auto first = kernel::makeBox(40, 30, 20);
    auto second = kernel::makeBox(40, 30, 20);
    REQUIRE(first.ok());
    REQUIRE(second.ok());

    TopoDS_Compound compound;
    BRep_Builder builder;
    builder.MakeCompound(compound);
    builder.Add(compound, occtOf(first.value().op.shape()));
    builder.Add(compound, occtOf(second.value().op.shape()));

    const auto path = std::filesystem::temp_directory_path() / "vcad_duplicate_faces.step";
    std::filesystem::remove(path);
    const auto registry = io::FormatRegistry::builtins();
    REQUIRE(io::exportFile(registry, path.string(), kernel::wrap(compound)));

    app::Controller controller;
    const auto imported = controller.importFile(path);
    if (!imported.ok()) INFO(imported.error().message);
    REQUIRE(imported.ok());

    const auto object = controller.document().find(imported.value());
    REQUIRE(object);
    INFO("state " << static_cast<int>(object->state()) << ": " << object->error().message);
    // Opened, with geometry the user can see, measure and export onward.
    CHECK(object->state() == document::ObjectState::Clean);
    REQUIRE(object->output() != nullptr);
    CHECK(object->output()->shape.volume() > 0.0);

    std::filesystem::remove(path);
}

// ── boundary siblings ──────────────────────────────────────────────────────────────────
//
// Edges and vertices are named by the faces they bound. Two that bound the SAME faces are
// siblings, and only geometry can separate them -- which ADR 0005 records as the main source of
// naming failure in this scheme.

namespace {

/// A straight edge between two points, as a raw shape.
TopoDS_Shape lineFrom(double x1, double y1, double z1, double x2, double y2, double z2) {
    BRepBuilderAPI_MakeEdge made(gp_Pnt(x1, y1, z1), gp_Pnt(x2, y2, z2));
    REQUIRE(made.IsDone());
    return made.Edge();
}

}   // namespace

TEST_CASE("siblings sharing a midpoint are separated by their length", "[naming][boundary]") {
    // A midpoint alone leaves these undecidable, and undecidable meant refused: the operation
    // failed with NamingLost over two edges that are obviously different things.
    using naming::internal::boundaryKeyOf;
    using naming::internal::canonicalOrder;
    using naming::internal::midpointOf;

    const auto shorter = lineFrom(-5, 0, 0, 5, 0, 0);
    const auto longer = lineFrom(-10, 0, 0, 10, 0, 0);
    REQUIRE(midpointOf(shorter) == midpointOf(longer));   // the case that used to have no answer

    const auto byMidpoint = canonicalOrder({shorter, longer}, midpointOf);
    CHECK(byMidpoint.anyAmbiguous());

    const auto byKey = canonicalOrder({shorter, longer}, boundaryKeyOf);
    CHECK_FALSE(byKey.anyAmbiguous());
    // Shorter first: the key's terms are compared in order, and length follows the midpoint.
    CHECK(byKey.elements[0].IsSame(shorter));
}

TEST_CASE("siblings sharing a midpoint AND a length are separated by their ends",
          "[naming][boundary]") {
    using naming::internal::boundaryKeyOf;
    using naming::internal::canonicalOrder;

    const auto across = lineFrom(-10, 0, 0, 10, 0, 0);
    const auto upright = lineFrom(0, -10, 0, 0, 10, 0);
    const auto ordered = canonicalOrder({across, upright}, boundaryKeyOf);
    CHECK_FALSE(ordered.anyAmbiguous());
}

TEST_CASE("the midpoint still decides first", "[naming][boundary]") {
    // The compatibility guarantee, and the reason the extra terms were appended rather than mixed
    // in. Wherever midpoints already differ the rest of the key is never consulted, so the order --
    // and therefore every name in every saved document -- is exactly what it was before.
    using naming::internal::boundaryKeyOf;
    using naming::internal::canonicalOrder;

    // The one nearer the origin is much longer, so a key that weighed length first would swap them.
    const auto nearAndLong = lineFrom(-40, 0, 0, 40, 0, 0);      // midpoint (0,0,0)
    const auto farAndShort = lineFrom(99, 0, 0, 101, 0, 0);      // midpoint (100,0,0)

    const auto ordered = canonicalOrder({farAndShort, nearAndLong}, boundaryKeyOf);
    REQUIRE(ordered.elements.size() == 2);
    CHECK(ordered.elements[0].IsSame(nearAndLong));
}

TEST_CASE("siblings that are genuinely the same shape are still refused", "[naming][boundary]") {
    // The richer key narrows what cannot be decided; it does not pretend the remainder away. Two
    // coincident duplicate edges agree on every term there is, and numbering them by whatever
    // std::sort happened to do would be the wrong kind of answer.
    using naming::internal::boundaryKeyOf;
    using naming::internal::canonicalOrder;

    const auto first = lineFrom(0, 0, 0, 10, 0, 0);
    const auto second = lineFrom(0, 0, 0, 10, 0, 0);
    const auto ordered = canonicalOrder({first, second}, boundaryKeyOf);
    CHECK(ordered.anyAmbiguous());
}

TEST_CASE("an edge's key does not depend on which way it runs", "[naming][boundary]") {
    // First and last vertex swap with orientation, and the same edge reversed is the same edge. A
    // key that moved with direction would reorder siblings for a reason that has nothing to do with
    // where they are.
    using naming::internal::boundaryKeyOf;

    const auto forward = lineFrom(0, 0, 0, 10, 5, 2);
    const auto backward = lineFrom(10, 5, 2, 0, 0, 0);
    CHECK(boundaryKeyOf(forward) == boundaryKeyOf(backward));
}

TEST_CASE("a washer's two circles are named, not refused", "[naming][boundary]") {
    // The case the richer key exists for, reached through real geometry rather than through
    // canonicalOrder directly -- so it pins that deriveBoundaries actually ASKS for that key.
    //
    // An annular face: the outer circle and the inner circle each bound this one face, which makes
    // them siblings, and both are centred on the same point. A midpoint alone cannot separate them,
    // so both were refused and naming a washer failed outright. Their lengths differ by exactly the
    // ratio of the radii, which the key's second term sees immediately.
    const gp_Pln plane(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1));
    const auto circleAt = [](double radius) {
        BRepBuilderAPI_MakeEdge edge(gp_Circ(gp_Ax2(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), radius));
        REQUIRE(edge.IsDone());
        BRepBuilderAPI_MakeWire wire(edge.Edge());
        REQUIRE(wire.IsDone());
        return wire.Wire();
    };

    BRepBuilderAPI_MakeFace face(plane, circleAt(20.0));
    REQUIRE(face.IsDone());
    face.Add(TopoDS::Wire(circleAt(10.0).Reversed()));
    REQUIRE(face.IsDone());
    const kernel::Shape washer = kernel::wrap(face.Face());

    // The premise: the two edges really do share a centre, so this is not a test that would pass
    // for some other reason.
    const auto edges = washer.subShapes(kernel::ShapeType::Edge);
    REQUIRE(edges.size() == 2);
    CHECK(naming::internal::midpointOf(occtOf(edges[0]))
          == naming::internal::midpointOf(occtOf(edges[1])));

    naming::NamingContext ctx(1, 0);
    const auto map = ctx.nameprimitive(washer, {});
    if (!map.ok()) INFO(map.error().detail);
    REQUIRE(map.ok());

    // Both circles named, and named differently -- a reference to the bore is not a reference to
    // the rim.
    const auto outerName = map.value().nameOf(edges[0]);
    const auto innerName = map.value().nameOf(edges[1]);
    REQUIRE(outerName.has_value());
    REQUIRE(innerName.has_value());
    CHECK_FALSE(*outerName == *innerName);
    CHECK(map.value().collisions().empty());
    CHECK(map.value().unnamed(washer).empty());
}
