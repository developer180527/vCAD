#pragma once

#include "cad/document/Document.h"
#include "cad/kernel/Result.h"
#include "cad/recompute/DdcCache.h"
#include "cad/render/RenderMesh.h"

#include <unordered_map>

namespace cad::render {

/// How finely to tessellate. Part of the cache key, so changing it invalidates correctly
/// rather than silently serving geometry at the wrong fidelity.
struct TessellationSettings {
    /// Chord deviation in millimetres. OCCT's linear deflection.
    double deflection = 0.05;
    /// Angular deviation in radians, which is what actually controls how round a cylinder
    /// looks. Tuning deflection alone gives faceted curves at every zoom level.
    double angularDeflection = 0.35;
    bool relativeToSize = true;   ///< scale deflection by the shape's bounding box

    [[nodiscard]] std::uint64_t digest() const noexcept;
};

/// Shape -> RenderMesh. Pure, deterministic, cacheable.
///
/// Deliberately NOT part of the swap seam (ADR 0007): the element map is ours, edge
/// extraction is ours, and a game renderer has no way to produce either. A replacement
/// renderer consumes RenderMesh; it does not build it.
kernel::Result<RenderMeshPtr> tessellate(const document::Output&, const TessellationSettings&);

/// Cached variant. Key = shape content hash + settings digest, in the same DDC that holds
/// cooked feature output — tessellation is exactly the kind of derived data it exists for.
class MeshCache {
public:
    explicit MeshCache(recompute::BlobStore& blobs);

    kernel::Result<RenderMeshPtr> get(const document::Output&, const TessellationSettings&);

    [[nodiscard]] std::size_t hits() const noexcept { return hits_; }
    [[nodiscard]] std::size_t misses() const noexcept { return misses_; }
    void resetStats() noexcept { hits_ = misses_ = 0; }

private:
    recompute::BlobStore& blobs_;
    /// L0: the same tiering argument as ADR 0004. Panning must not deserialise.
    std::unordered_map<std::uint64_t, RenderMeshPtr> live_;
    std::size_t hits_ = 0;
    std::size_t misses_ = 0;
};

}  // namespace cad::render
