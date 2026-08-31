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

    /// Names bound to more than one DISTINCT element. The other half of `unnamed`, and must also
    /// be empty for a healthy operation.
    ///
    /// One shape under two names is an alias and is legitimate — that is how a merge keeps both
    /// references alive. Two shapes under one name is not: `bind` keeps the last, so the earlier
    /// element becomes unreachable by exact resolution, and `resolveAll` returns before it consults
    /// the family list, so it reports one arbitrary winner rather than an ambiguity.
    ///
    /// Nothing produced this until copies arrived. A rigid transform reports its elements as
    /// `Modified` — the same element, moved — which is right for a MOVE and wrong for a COPY, so a
    /// pattern that translated a body and fused it back produced two faces with one name and no
    /// complaint from either safety net. Measured: two boxes fused, 52 bindings, 46 unique.
    [[nodiscard]] std::vector<ElementName> collisions() const;

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

    /// How much of the result must be nameable for the call to succeed.
    enum class Naming : std::uint8_t {
        /// Every element or nothing. For geometry WE built: if we cannot name a face we produced,
        /// something in the naming layer is wrong and the user must not build on it.
        Strict,

        /// Name what can be named, and report the rest as absent rather than as failure. For
        /// geometry we READ.
        ///
        /// A supplier's STEP file routinely carries duplicate or coincident faces -- junk left by
        /// whatever exported it -- which are genuinely indistinguishable by measurement. Under
        /// Strict those faces are unnamed and the whole import is refused, which means a defect in
        /// a corner of the part the user was never going to touch stops them opening the file at
        /// all. They usually want to look at it, measure it, and export it onward.
        ///
        /// The honesty is preserved where it matters: the ambiguous faces stay unnamed, so
        /// attaching a feature to one of them still fails, and says so, at the moment it is tried.
        BestEffort,
    };

    /// Names a primitive's faces from constructor-supplied tags (never from indices), then
    /// derives all lower-dimensional elements by boundary.
    kernel::Result<ElementMap> nameprimitive(const kernel::Shape& result,
                                             const std::vector<kernel::Shape>& taggedFaces,
                                             Naming = Naming::Strict);

    /// Propagates names across an operation.
    kernel::Result<ElementMap> propagate(const kernel::Operation& op,
                                         const std::vector<const kernel::Shape*>& inputs,
                                         const std::vector<const ElementMap*>& inputMaps);

    /// Where a copy sits in its pattern.
    ///
    /// Two coordinates, not one running number, and the difference is load-bearing: a rectangular
    /// pattern numbered 0..N flat renumbers EVERY instance when its row count changes, so a fillet
    /// on the fourth hole silently moves to a different hole. `(i, j)` does not — instance (1,2) is
    /// instance (1,2) whether the pattern is 3x3 or 3x8.
    ///
    /// Both are STEPS ALONG A DIRECTION, counted from the seed: `i` along the primary direction,
    /// `j` along the secondary one (0 for a linear or circular pattern). Defined that way so a
    /// direction FLIP keeps every instance's meaning — "three steps along the direction" still
    /// means that when the direction reverses. Encoding direction as the sign of the spacing
    /// instead would silently reassign instance 3 to a different physical copy, and any feature
    /// built on it would move for no visible reason.
    struct Instance {
        std::uint16_t i = 0;
        std::uint16_t j = 0;

        /// Packed into the single discriminator the name format already carries. 16 bits each is
        /// 65535 steps per direction, which is far past the point where a pattern is the wrong
        /// tool; the alternative was widening a persisted format for no reachable gain.
        [[nodiscard]] std::uint32_t key() const noexcept {
            return (static_cast<std::uint32_t>(i) << 16) | static_cast<std::uint32_t>(j);
        }

        friend bool operator==(const Instance&, const Instance&) = default;
    };

    /// Names a COPY of an already-named shape, so the copy is a different thing from its source.
    ///
    /// # Why propagate() cannot do this
    ///
    /// A rigid transform reports its elements as `Modified` — the SAME element, moved — which is
    /// exactly right for a MOVE and exactly wrong for a COPY. Propagating gives the copy its
    /// source's names verbatim, and a fresh feature serial does not separate them because the
    /// serial rides on names the operation MINTS, and a transform mints none. Fuse the two and you
    /// get one name bound to two faces: `bind` keeps the last, `resolveAll` returns on the exact
    /// hit before consulting the family, and "fillet the top face" rounds an arbitrary one of them
    /// with a green recompute. `ElementMap::collisions` refuses that, which makes a pattern built
    /// by propagate-and-fuse impossible rather than silently wrong — but refusing is not the same
    /// as working, and this is the half that works.
    ///
    /// # What it does
    ///
    /// Every name gets one more derivation step: `Generated` from the source element, carrying the
    /// instance coordinate as its discriminator. So the copy's top face is "the face generated from
    /// the source's top face, instance (1,2)" — distinct from every sibling, and still traceable to
    /// what it is a copy OF, which is what makes "the top face of every instance" answerable.
    ///
    /// Aliases survive as aliases: two source names for one element produce two names for the
    /// copy's corresponding element, related the same way. Flattening them would leave the copy
    /// with two independent bindings to one face and lose the merge history that made them one.
    ///
    /// The coordinate is the caller's and must differ between siblings; nothing here can check
    /// that, because two copies at the same coordinate are indistinguishable by construction. The
    /// ORIGINAL keeps its own map untouched — a copy is never the source, so even instance (0,0)
    /// produces different names, and references taken before the pattern still resolve.
    kernel::Result<ElementMap> nameCopy(const kernel::Operation& op, const kernel::Shape& source,
                                        const ElementMap& sourceMap, Instance);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace cad::naming
