#pragma once

/// Quantised measurements of a topological element.
///
/// # What these are for, and what they are not
///
/// Five places in the naming layer need to put otherwise-indistinguishable siblings into a
/// CANONICAL ORDER: the untagged faces of a primitive, the curves and points of an open sketch, the
/// new faces a boolean creates, and the pieces a split leaves behind. Ordering them by iteration
/// index would be worthless -- that is precisely the index-based identity ADR 0005 exists to
/// abolish -- so they are ordered by a measurement of the geometry itself.
///
/// Quantised to 1e-7 so the value is identical across processes and machines, which is what the
/// DDC's shared tier depends on.
///
/// **A measurement is a heuristic for MATCHING. It is never an IDENTITY.** Two different faces can
/// share an area and a centroid, and code that treats a metric as a key silently conflates them --
/// which has now happened three separate times in this module. Compare shapes with `IsSame`; use
/// these only to sort, and only where a tie is handled explicitly.

#include <TopoDS_Shape.hxx>

#include <algorithm>
#include <cstdint>
#include <tuple>
#include <vector>

namespace cad::naming::internal {

/// Quantise one coordinate or mass. The scale is the whole reason two machines agree.
[[nodiscard]] std::int64_t quantise(double) noexcept;

/// Area and centroid of a face, quantised.
struct FaceMetric {
    std::int64_t area = 0;
    std::int64_t cx = 0, cy = 0, cz = 0;

    bool operator==(const FaceMetric&) const = default;
    bool operator<(const FaceMetric& o) const {
        return std::tie(area, cx, cy, cz) < std::tie(o.area, o.cx, o.cy, o.cz);
    }
};

[[nodiscard]] FaceMetric measureFace(const TopoDS_Shape&);

/// Centroid of any element, quantised. Used to order boundary siblings.
struct Point3 {
    std::int64_t x = 0, y = 0, z = 0;

    bool operator==(const Point3&) const = default;
    bool operator<(const Point3& o) const {
        return std::tie(x, y, z) < std::tie(o.x, o.y, o.z);
    }
};

[[nodiscard]] Point3 midpointOf(const TopoDS_Shape&);

/// How boundary siblings are ordered: an edge that split, or the two edges a seam produces.
///
/// # Why more than a midpoint
///
/// Siblings here already share a parent set -- that is what makes them siblings -- so the only
/// thing separating them is geometry, and a midpoint alone leaves genuine cases undecided. Two
/// edges bounding the same faces with the same centre cannot be numbered, so both are refused and
/// the operation fails with NamingLost. ADR 0005 records midpoint ordering as the main source of
/// naming failure in this scheme, and this is the half of it that is fixable.
///
/// # Why the midpoint stays FIRST
///
/// Compatibility, and it is not negotiable. Wherever midpoints already differ, the extra terms are
/// never consulted and the order is exactly what it was -- so no name in any saved document moves.
/// The additional terms only decide cases that previously had no answer at all. A key that reordered
/// existing siblings would rename references in files people have, which is the one thing this layer
/// must not do casually.
struct BoundaryKey {
    Point3 midpoint;        ///< first, and usually decisive
    std::int64_t extent{};  ///< length for an edge; zero for a vertex
    Point3 low, high;       ///< endpoints, sorted, so the key does not depend on orientation
    int curve{};            ///< GeomAbs_CurveType, so a line and an arc are never confused

    friend bool operator==(const BoundaryKey&, const BoundaryKey&) = default;
    bool operator<(const BoundaryKey& o) const {
        return std::tie(midpoint, extent, low, high, curve)
               < std::tie(o.midpoint, o.extent, o.low, o.high, o.curve);
    }
};

[[nodiscard]] BoundaryKey boundaryKeyOf(const TopoDS_Shape&);

/// Siblings put into a canonical order, with the ones that could not be separated marked.
struct CanonicalOrder {
    std::vector<TopoDS_Shape> elements;    ///< sorted; ALL of them, ambiguous included
    std::vector<std::uint8_t> ambiguous;   ///< parallel to `elements`; 1 = ties with a neighbour

    [[nodiscard]] bool anyAmbiguous() const {
        return std::find(ambiguous.begin(), ambiguous.end(), std::uint8_t{1}) != ambiguous.end();
    }
};

/// Orders otherwise-indistinguishable siblings so that each can be given a persistent index.
///
/// # Why this is one function and not five copies
///
/// Five places in this module needed "sort by a measurement, use the position as the
/// discriminator", and each wrote it out longhand: the untagged faces of a primitive, the curves
/// and the points of an open sketch, the new faces of a boolean, and the pieces of a split. Four of
/// the five were missing the same check, and the fifth only had it because the bug had just been
/// found in the fourth.
///
/// # The check
///
/// `std::sort` leaves equal elements in an UNSPECIFIED order. Taking a persistent name from that
/// order means that when two siblings measure the same, which one is "piece 1" is decided by
/// whatever the standard library happened to do -- silently, and potentially differently on another
/// machine or another build. Since these names are written into user documents and into the DDC's
/// keys, that is not a theoretical concern.
///
/// So tied elements are MARKED rather than numbered, and every caller skips them: they end up
/// unnamed, `ElementMap::unnamed` reports it, and the operation fails with NamingLost. ADR 0005 is
/// explicit that a reference resolving to the WRONG element is worse than one that fails.
///
/// # Why the marking is per element and not per call
///
/// Because one bad pair must not cost the rest their names. A supplier's STEP file with two
/// duplicate faces in it -- common, and usually junk left by the exporter -- would otherwise leave
/// every OTHER face of the part unnamed too, and the import would be refused over geometry the user
/// was never going to touch. The unambiguous elements keep their positions in the sorted list, so
/// their names do not move when a tie elsewhere appears or goes away.
template <class Measure>
[[nodiscard]] CanonicalOrder canonicalOrder(std::vector<TopoDS_Shape> elements, Measure measure) {
    std::sort(elements.begin(), elements.end(),
              [&measure](const TopoDS_Shape& a, const TopoDS_Shape& b) {
                  return measure(a) < measure(b);
              });
    std::vector<std::uint8_t> ambiguous(elements.size(), 0);
    for (std::size_t i = 1; i < elements.size(); ++i) {
        if (measure(elements[i - 1]) == measure(elements[i])) {
            ambiguous[i - 1] = 1;
            ambiguous[i] = 1;
        }
    }
    return CanonicalOrder{std::move(elements), std::move(ambiguous)};
}

}  // namespace cad::naming::internal
