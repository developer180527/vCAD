#pragma once

/// Everything needed to compute a document and turn it into meshes, wired once.
///
/// # Why this exists
///
/// Two applications were built over one core. `app::Controller`, which both shells run, and
/// `abi::Session`, which the Rust suite drives, each assembled the same stack by hand: the feature
/// registry, a recompute cache, a blob store, and the mesh cache over that store. Neither was
/// layered on the other, so the wiring could drift with nothing to notice -- and the deepest tests
/// in the project (concurrency, property invariants, torture, scaling, rollback, persistence) run
/// against the second one, which makes them evidence about the product only for as long as the two
/// agree. `tests/acceptance/two_paths_agree.cpp` checks that they still do.
///
/// # The two had already drifted, in both directions
///
/// Extracting this found it. `Controller` hard-coded `MemoryCache` + `MemoryBlobStore`, so the
/// SHELLS had no disk cache tier at all: nothing a user computed was ever served from disk, and the
/// whole point of a content-addressed cache -- that CI or a colleague's machine can have done the
/// work already -- was unavailable in the product. `abi::Session` had the better wiring and used a
/// `TieredCache` over `MemoryCache` and `DdcCache`.
///
/// The first version of this class then made the opposite mistake: it used the `DdcCache` alone,
/// which would have taken the L0 memory tier AWAY from the ABI session and sent every cache lookup
/// to disk. The tiering below is the ABI's, kept deliberately: L0 answers the lookups that
/// interaction depends on, L1 is what survives a restart and is shared.
///
/// `DdcCache` implements both `recompute::Cache` and `recompute::BlobStore`, which is what lets a
/// tessellated mesh reach the same shared tier as a cooked shape. So on the disk path it is the L1
/// of the cache AND the blob store, while the `TieredCache` owns it. The memory path needs two
/// separate objects instead. Each caller that wired this by hand had to know all of that; now one
/// constructor argument decides it.
///
/// # What it deliberately does not own
///
/// No document, no history, no camera, no renderer backend. Those differ legitimately between a
/// shell driving one open document and a C session serving a plugin host, and folding them in here
/// would force one shape onto both. This owns the parts that were identical.

#include "cad/recompute/DdcCache.h"
#include "cad/recompute/Engine.h"
#include "cad/render/Tessellate.h"

#include <filesystem>
#include <memory>

namespace cad::runtime {

class DocumentRuntime {
public:
    /// An EMPTY path means memory tiers only: results live as long as this object. That is what a
    /// test wants, and it is what both callers did before this class existed.
    ///
    /// Otherwise the on-disk DDC tier is created at `cacheDir` and becomes the cache's L1 and the
    /// mesh blob store, so a result computed on another machine -- or by CI -- is served rather
    /// than recomputed.
    explicit DocumentRuntime(std::filesystem::path cacheDir = {});

    ~DocumentRuntime();
    DocumentRuntime(const DocumentRuntime&) = delete;
    DocumentRuntime& operator=(const DocumentRuntime&) = delete;

    /// The built-in feature types. Mutable because a plugin host registers into it -- which is
    /// today only true of the ABI's session; see ADR 0011's note on plugin features not reaching
    /// the application.
    [[nodiscard]] recompute::FeatureRegistry& registry() noexcept { return registry_; }
    [[nodiscard]] const recompute::FeatureRegistry& registry() const noexcept { return registry_; }

    [[nodiscard]] recompute::Cache& cache() noexcept { return *cache_; }
    [[nodiscard]] recompute::BlobStore& blobs() noexcept { return *blobs_; }
    [[nodiscard]] render::MeshCache& meshes() noexcept { return *meshes_; }

    /// Whether computed results and meshes also persist to disk. Reported rather than inferred by
    /// a caller, so it can be said out loud in a status line or a support bundle.
    [[nodiscard]] bool onDisk() const noexcept { return ddc_ != nullptr; }

private:
    recompute::FeatureRegistry registry_;

    /// `MemoryCache` on the memory path; `TieredCache` over memory and the DDC on the disk one.
    std::unique_ptr<recompute::Cache> cache_;

    /// Memory path only: the blob store has no tiering to do.
    std::unique_ptr<recompute::MemoryBlobStore> memoryBlobs_;

    /// NON-OWNING, and the comment matters: on the disk path the `TieredCache` above owns this.
    /// An earlier version of the same wiring in abi/src/Session.cpp held it in a second unique_ptr
    /// as well, which compiles perfectly and double-frees on release.
    recompute::DdcCache* ddc_ = nullptr;

    /// Whichever of the above serves blobs. Non-owning: which one is holding them is exactly the
    /// detail this class exists to stop spreading.
    recompute::BlobStore* blobs_ = nullptr;

    /// After the blob store, because it holds a reference to it.
    std::unique_ptr<render::MeshCache> meshes_;
};

}  // namespace cad::runtime
