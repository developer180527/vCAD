#pragma once

#include "cad/kernel/Result.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// TopoDS_Shape is deliberately NOT in this header. OCCT types never appear in a core public
// header — that is what makes the C plugin ABI and the Swift bridge possible. The handle
// lives behind Shape::Impl, declared in cad/kernel/internal/Occt.h, which only core .cpp
// files (kernel and naming) may include.

namespace cad::kernel {

enum class ShapeType : std::uint8_t {
    Compound, CompSolid, Solid, Shell, Face, Wire, Edge, Vertex, Unknown
};

const char* toString(ShapeType) noexcept;

/// 256-bit content hash. Placeholder implementation until assetlib lands at M2 and brings
/// BLAKE3-256 with it; the interface is what matters here.
struct ShapeHash {
    std::uint64_t lanes[4]{};

    [[nodiscard]] std::string hex() const;

    /// A 64-bit key over ALL FOUR lanes.
    ///
    /// Use this, never `lanes[0]`. The lanes are not interchangeable: naming::contentHash mixes
    /// element names into lane 0 and geometry (areas, centroids) into lanes 1-3, so keying on
    /// lane 0 alone is keying on names only. That bug shipped briefly in the mesh cache: two
    /// boxes with the same face names but different dimensions produced the same key, so
    /// resizing a part served the mesh for its old size. Caught by an M3.2 scene test.
    [[nodiscard]] std::uint64_t fold64() const noexcept {
        std::uint64_t h = 1469598103934665603ULL;
        for (const std::uint64_t lane : lanes) {
            for (int i = 0; i < 8; ++i) {
                h ^= (lane >> (i * 8)) & 0xFFu;
                h *= 1099511628211ULL;
            }
        }
        return h;
    }

    friend bool operator==(const ShapeHash&, const ShapeHash&) = default;
};

/// Value wrapper over TopoDS_Shape. Copies are cheap (OCCT shapes are handle-based) and
/// share underlying topology, exactly like TopoDS_Shape itself.
class Shape {
public:
    Shape();
    ~Shape();
    Shape(const Shape&);
    Shape(Shape&&) noexcept;
    Shape& operator=(const Shape&);
    Shape& operator=(Shape&&) noexcept;

    [[nodiscard]] bool isNull() const noexcept;
    [[nodiscard]] ShapeType type() const;

    /// TopoDS_Shape::IsSame — same underlying TShape and location, ignoring orientation.
    /// This is identity WITHIN one shape tree only. It is NOT stable across a rebuild:
    /// that is what cad::naming exists for.
    [[nodiscard]] bool isSame(const Shape&) const;

    /// Sub-shapes in OCCT traversal order, deduplicated. The index here is not a persistent
    /// identity; use this for iteration only.
    [[nodiscard]] std::vector<Shape> subShapes(ShapeType of) const;

    /// BRepCheck_Analyzer gate. Run on every operation output and every import.
    [[nodiscard]] Result<void> validate() const;

    /// Mass and centre of mass — area/centroid for a face, length/midpoint for an edge.
    /// Used by the naming layer's geometric fallback and, heavily, by diagnostics.
    struct Measure {
        double mass = 0.0;
        double cx = 0.0, cy = 0.0, cz = 0.0;
    };
    [[nodiscard]] Measure measure() const;

    /// Enclosed volume in mm³, or 0 for a shape that encloses nothing. Cheap, exact, and
    /// the single most useful assertion in a geometry test: it catches a boolean that
    /// silently did nothing far more reliably than a face count.
    [[nodiscard]] double volume() const;

    struct Impl;
    [[nodiscard]] const Impl& impl() const noexcept { return *impl_; }
    [[nodiscard]] Impl& impl() noexcept { return *impl_; }

private:
    std::unique_ptr<Impl> impl_;
};

/// An OCCT modelling algorithm, kept alive so the naming layer can interrogate its
/// Generated/Modified/IsDeleted reports. Every shape-producing kernel call returns one of
/// these rather than a bare Shape, because the algorithm IS the naming evidence and it is
/// destroyed the moment you drop it.
class Operation {
public:
    Operation();
    ~Operation();
    Operation(Operation&&) noexcept;
    Operation& operator=(Operation&&) noexcept;
    Operation(const Operation&) = delete;
    Operation& operator=(const Operation&) = delete;

    [[nodiscard]] Shape shape() const;

    struct Impl;
    [[nodiscard]] const Impl& impl() const noexcept { return *impl_; }
    [[nodiscard]] Impl& impl() noexcept { return *impl_; }

private:
    std::unique_ptr<Impl> impl_;
};

/// Which sub-shapes to enumerate. Values are the kernel's own; the ABI maps its CAD_SUB_*
/// constants onto these.
enum class SubShape : std::uint8_t { Face, Edge, Vertex };

/// The sub-shapes of one kind, deduplicated, in a stable order.
///
/// Deduplicated because OCCT's explorer visits a face once per shell that references it, so a
/// solid's face list would otherwise contain repeats — and a caller iterating "every face" would
/// process the same face twice believing they were different.
///
/// The order is stable for a given shape and carries NO meaning across edits. It exists so a
/// caller can reach every face once; the durable reference is the element NAME obtained from the
/// shape's ElementMap, never the index. Lives here rather than in abi/ so the ABI layer needs no
/// OCCT headers — the same reason every other topology question is answered by the kernel.
[[nodiscard]] std::vector<Shape> subShapes(const Shape&, SubShape);

/// A planar face measured into an origin and two in-plane axes.
///
/// The bridge between "a sketch is drawn on this face" and "the sketch's 2D coordinates mean these
/// 3D points". Lives in `core/kernel` because measuring a face needs OCCT, and nothing above the
/// kernel is allowed to touch it — `core/sketch` in particular holds only the face's NAME.
struct PlaneFrame {
    double origin[3]{0, 0, 0};
    double u[3]{1, 0, 0};
    double v[3]{0, 1, 0};
    double normal[3]{0, 0, 1};
};

/// Measures a planar face.
///
/// Fails, rather than approximating, when the face is not planar. A cylindrical or spline face has
/// no single plane, and picking one would place a sketch somewhere the user did not choose — the
/// same class of silent misplacement the sketch's own `isPlaced()` check exists to prevent.
///
/// The axes come from OCCT's own parameterisation of the plane, so they are stable for a given
/// face rather than derived from an arbitrary choice here: two calls on the same face agree, which
/// is what stops a sketch rotating on its own face between rebuilds.
[[nodiscard]] Result<PlaneFrame> planeOf(const Shape& face);

}  // namespace cad::kernel
