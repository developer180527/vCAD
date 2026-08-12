#pragma once

// CPU-side render data. No GPU types, no OCCT types.
//
// A RenderMesh is a pure function of (shape content hash, tessellation settings), which is
// why it lives in the DDC alongside cooked feature output (ADR 0004). Tessellating a
// 40,000-face assembly is seconds; doing it once per machine instead of once per open is the
// difference between a tool that feels instant and one that does not.

#include "cad/kernel/Shape.h"
#include "cad/naming/ElementName.h"

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace cad::render {

/// 28 bytes. Deliberately not the engine's 48-byte Vertex: tangent and uv are dead weight
/// for untextured CAD geometry, and bandwidth is the binding constraint on the one thing we
/// have a great deal of. See ADR 0007 decision 4.
struct CadVertex {
    float position[3];
    float normal[3];
    /// Index into RenderMesh::elements. This is what makes GPU ID-buffer picking possible,
    /// and it has no equivalent in a game renderer.
    std::uint32_t element;
};
static_assert(sizeof(CadVertex) == 28, "CadVertex must stay tightly packed for upload");

/// One drawable run of triangles belonging to a single face.
struct FaceRange {
    std::uint32_t indexOffset = 0;
    std::uint32_t indexCount = 0;
    std::uint32_t element = 0;      ///< index into RenderMesh::elements
};

/// One polyline. CAD edges are exact curves from OCCT, sampled to a chord tolerance — NOT
/// derived from the triangle mesh, and NOT a screen-space effect. Crisp edges are the single
/// biggest difference between "a 3D view" and "a CAD viewport" (ADR 0007 decision 5).
struct EdgeRange {
    std::uint32_t vertexOffset = 0;
    std::uint32_t vertexCount = 0;
    std::uint32_t element = 0;
};

struct Bounds {
    float min[3]{};
    float max[3]{};
    [[nodiscard]] bool valid() const noexcept { return min[0] <= max[0]; }
};

/// Immutable once built. Shared by `shared_ptr`, never copied — that, plus upload keyed by
/// contentHash, is the whole of our "shared memory" story (ADR 0007 decision 3).
class RenderMesh {
public:
    std::vector<CadVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<FaceRange> faces;

    /// Edge geometry, in its own arrays: line primitives, drawn in a separate pass.
    std::vector<float> edgeVertices;          ///< xyz triples
    std::vector<EdgeRange> edges;

    /// Element names, parallel to the `element` indices above. This is how a GPU pick
    /// becomes a stable geometric reference rather than a triangle number.
    std::vector<naming::ElementName> elements;

    Bounds bounds;

    /// Content hash of the source shape plus the tessellation settings. The upload key.
    kernel::ShapeHash contentHash;

    [[nodiscard]] std::size_t triangleCount() const noexcept { return indices.size() / 3; }
    [[nodiscard]] bool empty() const noexcept { return vertices.empty() && edgeVertices.empty(); }
};

using RenderMeshPtr = std::shared_ptr<const RenderMesh>;

}  // namespace cad::render
