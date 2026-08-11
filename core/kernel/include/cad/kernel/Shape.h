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

}  // namespace cad::kernel
