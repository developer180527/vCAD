/// What a shape's content hash actually describes.
///
/// # Why this matters more than it looks
///
/// This hash is not the recompute cache key -- the DDC keys on a feature's INPUTS, so a collision
/// here has never been able to serve wrong cached geometry. It feeds two other things:
///
///   * `serialForShapes` in the C ABI, which derives an operation's naming serial from its input
///     shapes. Two inputs that hash alike mint the same names for different geometry.
///   * every determinism check in the project -- the cross-process test, the ABI's run-it-twice
///     self-check. Those assert "same hash, same result", which is only ever as strong as the
///     description underneath. A hash that cannot see topology passes two shapes that differ in
///     topology, and reports that as determinism.
///
/// It used to describe FACES only, and each face only by its name, area and centroid. These tests
/// state what it has to be able to tell apart.

#include "cad/kernel/Booleans.h"
#include "cad/kernel/Primitives.h"
#include "cad/kernel/Transform.h"
#include "cad/kernel/internal/Occt.h"
#include "cad/naming/ElementMap.h"

#include <catch2/catch_test_macros.hpp>

#include <BRepBuilderAPI_MakeEdge.hxx>
#include <TopoDS_Edge.hxx>
#include <BRep_Builder.hxx>
#include <GC_MakeArcOfCircle.hxx>
#include <gp_Pnt.hxx>
#include <TopoDS_Compound.hxx>

#include <string>

using namespace cad;
using naming::contentHash;

namespace {

struct Named {
    kernel::Shape shape;
    naming::ElementMap map;
};

Named boxNamed(double dx, double dy, double dz, std::uint32_t serial = 1) {
    auto made = kernel::makeBox(dx, dy, dz);
    REQUIRE(made.ok());
    naming::NamingContext ctx(serial, 0);
    auto map = ctx.nameprimitive(made.value().op.shape(), made.value().taggedFaces);
    REQUIRE(map.ok());
    return Named{made.value().op.shape(), std::move(map.value())};
}

std::string hashOf(const Named& n) { return contentHash(n.shape, n.map).hex(); }

}   // namespace

TEST_CASE("the same shape hashes the same way twice", "[naming][contenthash]") {
    // The property everything else rests on. Built independently, so agreement means the hash is a
    // function of the shape rather than of the object it was computed from.
    CHECK(hashOf(boxNamed(40, 30, 20)) == hashOf(boxNamed(40, 30, 20)));
}

TEST_CASE("different geometry hashes differently", "[naming][contenthash]") {
    CHECK(hashOf(boxNamed(40, 30, 20)) != hashOf(boxNamed(41, 30, 20)));
    CHECK(hashOf(boxNamed(40, 30, 20)) != hashOf(boxNamed(40, 30, 21)));
}

TEST_CASE("a shape and a piece of it do not hash alike", "[naming][contenthash][structure]") {
    // A box against its own faces held loose in a compound. Every FACE is identical -- same names,
    // same areas, same centroids -- and the two are completely different things: one is a solid
    // with edges and corners, the other is a bag of surfaces. Describing faces alone cannot tell
    // them apart, which is the whole reason edges, vertices and counts are now in the hash.
    const auto solid = boxNamed(40, 30, 20);

    TopoDS_Compound compound;
    BRep_Builder builder;
    builder.MakeCompound(compound);
    for (const auto& face : solid.shape.subShapes(kernel::ShapeType::Face)) {
        builder.Add(compound, kernel::occt(const_cast<kernel::Shape&>(face)));
    }
    const kernel::Shape loose = kernel::wrap(compound);

    // Named with the SAME map, so the face names really are identical between the two.
    INFO("solid " << hashOf(solid));
    INFO("loose " << contentHash(loose, solid.map).hex());
    CHECK(hashOf(solid) != contentHash(loose, solid.map).hex());
}

TEST_CASE("a shape with an extra body does not hash alike", "[naming][contenthash][structure]") {
    // Counts, on their own. The second box's faces are unnamed -- the map knows nothing about them
    // -- so the per-element loop skips every one of them and would have said nothing at all.
    const auto one = boxNamed(40, 30, 20);

    auto second = kernel::makeBox(40, 30, 20);
    REQUIRE(second.ok());
    TopoDS_Compound compound;
    BRep_Builder builder;
    builder.MakeCompound(compound);
    builder.Add(compound, kernel::occt(const_cast<kernel::Shape&>(one.shape)));
    builder.Add(compound, kernel::occt(second.value().op.shape()));

    CHECK(hashOf(one) != contentHash(kernel::wrap(compound), one.map).hex());
}

TEST_CASE("moving a shape changes its hash", "[naming][contenthash]") {
    // Centroids move, so this held before too. Kept because it is the case a user would assume is
    // covered, and because it fails loudly if the positional terms are ever dropped.
    const auto still = boxNamed(40, 30, 20);
    auto moved = kernel::translate(still.shape, 100.0, 0.0, 0.0);
    REQUIRE(moved.ok());
    naming::NamingContext ctx(2, 0);
    auto map = ctx.propagate(moved.value(), {&still.shape}, {&still.map});
    REQUIRE(map.ok());
    CHECK(hashOf(still) != contentHash(moved.value().shape(), map.value()).hex());
}

TEST_CASE("a null shape hashes to nothing rather than misbehaving", "[naming][contenthash]") {
    naming::ElementMap empty;
    const kernel::Shape null;
    CHECK(contentHash(null, empty).hex() == kernel::ShapeHash{}.hex());
}

TEST_CASE("two face-less shapes are told apart", "[naming][contenthash][structure]") {
    // The case where describing faces alone says NOTHING. An open sketch -- a revolve axis, a path,
    // an unclosed profile -- has no faces at all, so before edges and vertices were in the hash two
    // completely different curve sets produced the same digest, and every determinism check over
    // them was comparing one constant to another.
    const auto curves = [](double length) {
        auto made = kernel::makeBox(length, 30, 20);
        REQUIRE(made.ok());
        TopoDS_Compound compound;
        BRep_Builder builder;
        builder.MakeCompound(compound);
        for (const auto& e : made.value().op.shape().subShapes(kernel::ShapeType::Edge)) {
            builder.Add(compound, kernel::occt(const_cast<kernel::Shape&>(e)));
        }
        const kernel::Shape shape = kernel::wrap(compound);
        naming::NamingContext ctx(1, 0);
        auto map = ctx.nameprimitive(shape, {});
        REQUIRE(map.ok());
        return Named{shape, std::move(map.value())};
    };

    const auto shorter = curves(40);
    const auto longer = curves(80);
    REQUIRE(shorter.shape.subShapes(kernel::ShapeType::Face).empty());
    CHECK(hashOf(shorter) != hashOf(longer));
}

TEST_CASE("orientation is part of what a shape is", "[naming][contenthash][structure]") {
    // The same face, forward and reversed. Same name, same area, same centroid -- and the two point
    // in opposite directions, which for a face is the difference between inside and outside.
    const auto solid = boxNamed(40, 30, 20);
    const auto faces = solid.shape.subShapes(kernel::ShapeType::Face);
    REQUIRE_FALSE(faces.empty());
    const TopoDS_Shape& raw = kernel::occt(const_cast<kernel::Shape&>(faces[0]));

    const auto compoundOf = [](const TopoDS_Shape& face) {
        TopoDS_Compound compound;
        BRep_Builder builder;
        builder.MakeCompound(compound);
        builder.Add(compound, face);
        return kernel::wrap(compound);
    };

    const auto forward = compoundOf(raw);
    const auto reversed = compoundOf(raw.Reversed());
    CHECK(contentHash(forward, solid.map).hex() != contentHash(reversed, solid.map).hex());
}

TEST_CASE("one name on two kinds of element does not hash alike",
          "[naming][contenthash][structure]") {
    // Records are keyed by name, and a name is unique WITHIN a shape -- so the element's own type
    // only earns its place ACROSS two shapes, where the same name can sit on a face in one and on
    // an edge in the other. Contrived to build, and the exact thing the type field guards.
    const auto solid = boxNamed(40, 30, 20);
    const auto faces = solid.shape.subShapes(kernel::ShapeType::Face);
    const auto edges = solid.shape.subShapes(kernel::ShapeType::Edge);
    REQUIRE_FALSE(faces.empty());
    REQUIRE_FALSE(edges.empty());

    naming::NameStep step;
    step.featureSerial = 99;
    step.provenance = naming::Provenance::Primitive;
    const naming::ElementName shared({step});

    const auto singleton = [&shared](const kernel::Shape& element) {
        TopoDS_Compound compound;
        BRep_Builder builder;
        builder.MakeCompound(compound);
        builder.Add(compound, kernel::occt(const_cast<kernel::Shape&>(element)));
        naming::ElementMap map;
        map.bind(element, shared);
        return std::pair{kernel::wrap(compound), std::move(map)};
    };

    const auto [faceShape, faceMap] = singleton(faces[0]);
    const auto [edgeShape, edgeMap] = singleton(edges[0]);
    CHECK(contentHash(faceShape, faceMap).hex() != contentHash(edgeShape, edgeMap).hex());
}

TEST_CASE("an edge is measured by its length, not by its area",
          "[naming][contenthash][structure]") {
    // A straight edge and a curved one between the SAME two points. Identical vertices, identical
    // endpoints, different length -- so the only thing that can tell them apart is the edge's own
    // measurement.
    //
    // Worth pinning because measuring an edge with SURFACE properties returns zero for every edge,
    // and that exact mistake was shipping elsewhere in this module: it made an open sketch's curves
    // all measure alike, so they were named by traversal order instead.
    const gp_Pnt from(0.0, 0.0, 0.0);
    const gp_Pnt to(100.0, 0.0, 0.0);
    const gp_Pnt bulge(50.0, 30.0, 0.0);

    BRepBuilderAPI_MakeEdge straight(from, to);
    REQUIRE(straight.IsDone());
    const auto arc = GC_MakeArcOfCircle(from, bulge, to);
    REQUIRE(arc.IsDone());
    BRepBuilderAPI_MakeEdge curved(arc.Value());
    REQUIRE(curved.IsDone());

    const auto named = [](const TopoDS_Shape& edge) {
        TopoDS_Compound compound;
        BRep_Builder builder;
        builder.MakeCompound(compound);
        builder.Add(compound, edge);
        const kernel::Shape shape = kernel::wrap(compound);
        naming::NamingContext ctx(1, 0);
        auto map = ctx.nameprimitive(shape, {});
        REQUIRE(map.ok());
        return Named{shape, std::move(map.value())};
    };

    CHECK(hashOf(named(straight.Edge())) != hashOf(named(curved.Edge())));
}
