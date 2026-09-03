#pragma once

#include "cad/document/Document.h"

#include <span>
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
    ///
    /// 0.20 rad is 11.5 degrees, so a full circle gets about 31 segments. It was 0.35 -- 20
    /// degrees, 18 segments -- which is coarse enough to see without looking for it: a fillet
    /// rounding a cylinder into a dome came out with a visibly straight-edged silhouette, which
    /// was reported as the renderer being broken rather than as a setting.
    ///
    /// Measured on that exact shape, a 24 mm fillet on a 25 mm cylinder: 0.35 gives 1,436
    /// triangles in 9.9 ms, 0.20 gives 4,280 in 32 ms, 0.10 gives 16,628 in 153 ms. The cost
    /// roughly doubles per step, so this is the last one that is nearly free and the next one is
    /// not.
    ///
    /// It does NOT make a close-up smooth, and no fixed value can: at any tessellation there is a
    /// zoom that shows the facets. That needs view-dependent quality — re-tessellating as a part
    /// grows on screen — which is a feature and not a constant, and which needs recompute to stop
    /// being synchronous first, because 153 ms on the UI thread per zoom step is its own problem.
    double angularDeflection = 0.20;
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

    /// Tessellates everything in `outputs` that is not already cached, in parallel.
    ///
    /// Tessellation dominates opening a document: measured at roughly 2.2 ms per mesh, so 20,000
    /// unique parts cost 44.7 s of blocking work before anything appeared, and 100,000 would be
    /// over three minutes. It is also the one part of the pipeline that parallelises cleanly —
    /// `tessellate` is a pure function of a shape and the settings, which is what ADR 0007 means by
    /// calling it cacheable.
    ///
    /// The CACHE is never touched concurrently. Missing keys are found on this thread, the meshes
    /// are built on worker threads into a local vector, and the results are inserted here after the
    /// workers have joined. Concurrency is therefore confined to `tessellate` itself, and the
    /// cache's own invariants cannot be raced.
    ///
    /// Returns the number of meshes actually built. Calling `get` afterwards is a hit for each.
    std::size_t warm(std::span<const document::Output* const>, const TessellationSettings&);

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
