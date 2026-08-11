#pragma once

#include "cad/kernel/Result.h"
#include "cad/kernel/Shape.h"
#include "cad/naming/ElementName.h"

#include <memory>
#include <optional>
#include <vector>

namespace cad::naming {

/// Bidirectional association between a shape's topological sub-elements and their stable
/// names. Travels with the shape through the document, is serialized with it, and
/// participates in its content hash (ADR 0005).
class ElementMap {
public:
    ElementMap();
    ~ElementMap();
    ElementMap(const ElementMap&);
    ElementMap(ElementMap&&) noexcept;
    ElementMap& operator=(const ElementMap&);
    ElementMap& operator=(ElementMap&&) noexcept;

    void bind(const kernel::Shape& sub, ElementName name);

    /// Record that `also` refers to the same element as `canonical`. Used for merges.
    void alias(const ElementName& canonical, ElementName also);

    [[nodiscard]] std::optional<ElementName> nameOf(const kernel::Shape& sub) const;

    /// Exact resolution. Empty if the element no longer exists — callers must treat that as
    /// a failure and must not substitute a neighbour.
    [[nodiscard]] std::optional<kernel::Shape> resolve(const ElementName&) const;
    [[nodiscard]] std::optional<kernel::Shape> resolve(const ElementId&) const;

    /// Resolves a name to every element in its family — i.e. including all children if the
    /// element was split since the reference was taken. This is what feature code should
    /// call: "fillet this edge" means the whole edge, even if it is now two edges.
    [[nodiscard]] std::vector<kernel::Shape> resolveAll(const ElementName&) const;

    [[nodiscard]] std::vector<ElementName> allNames() const;
    [[nodiscard]] std::size_t size() const noexcept;

    /// Contribution to the owning shape's content hash.
    [[nodiscard]] std::uint64_t digest() const noexcept;

    /// Sub-elements of `owner` (faces, edges, vertices) that have no name. Must be empty for
    /// a healthy operation; non-empty is what raises ErrorCode::NamingLost, at the moment it
    /// happens rather than later.
    [[nodiscard]] std::vector<kernel::Shape> unnamed(const kernel::Shape& owner) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// Full content hash of a shape: geometry, topology, and — per ADR 0005 — its element map.
/// Faces are visited in element-name order, never in OCCT traversal order, so the result is
/// identical across processes and machines. That determinism is what makes the DDC's shared
/// tier worth anything.
[[nodiscard]] kernel::ShapeHash contentHash(const kernel::Shape&, const ElementMap&);

/// Builds the output element map for one modelling operation.
///
/// Strategy, in order:
///   1. Name the result's FACES by interrogating the algorithm's Modified()/Generated()
///      reports against every input face and edge.
///   2. Fall back to geometric matching for faces the algorithm does not report. OCCT's
///      reporting is genuinely incomplete for booleans and offsets; this is not optional.
///   3. Name every EDGE and VERTEX by the set of faces it bounds (Provenance::Boundary),
///      disambiguating same-boundary siblings by canonical midpoint order.
class NamingContext {
public:
    NamingContext(std::uint32_t featureSerial, std::uint16_t opTag);
    ~NamingContext();
    NamingContext(const NamingContext&) = delete;
    NamingContext& operator=(const NamingContext&) = delete;

    /// Names a primitive's faces from constructor-supplied tags (never from indices), then
    /// derives all lower-dimensional elements by boundary.
    kernel::Result<ElementMap> nameprimitive(const kernel::Shape& result,
                                             const std::vector<kernel::Shape>& taggedFaces);

    /// Propagates names across an operation.
    kernel::Result<ElementMap> propagate(const kernel::Operation& op,
                                         const std::vector<const kernel::Shape*>& inputs,
                                         const std::vector<const ElementMap*>& inputMaps);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace cad::naming
