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

/// Siblings put into a canonical order, and whether the measurement actually separated them.
struct CanonicalOrder {
    std::vector<TopoDS_Shape> elements;   ///< sorted; empty when `ambiguous`
    bool ambiguous = false;               ///< two or more measured identically
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
/// So a tie clears the list: the caller cannot number what it cannot tell apart. Every caller then
/// leaves those elements sharing one name, `ElementMap::collisions` reports it, and the operation
/// fails with NamingLost. ADR 0005 is explicit that a reference resolving to the WRONG element is
/// worse than one that fails.
template <class Measure>
[[nodiscard]] CanonicalOrder canonicalOrder(std::vector<TopoDS_Shape> elements, Measure measure) {
    std::sort(elements.begin(), elements.end(),
              [&measure](const TopoDS_Shape& a, const TopoDS_Shape& b) {
                  return measure(a) < measure(b);
              });
    const bool ambiguous =
        std::adjacent_find(elements.begin(), elements.end(),
                           [&measure](const TopoDS_Shape& a, const TopoDS_Shape& b) {
                               return measure(a) == measure(b);
                           }) != elements.end();
    if (ambiguous) return CanonicalOrder{{}, true};
    return CanonicalOrder{std::move(elements), false};
}

}  // namespace cad::naming::internal
