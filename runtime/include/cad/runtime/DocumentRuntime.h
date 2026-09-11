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
/// This is the other half of that fix: the stack stops being assembled twice.
///
/// # The pairing that made it worth extracting
///
/// `DdcCache` implements BOTH `recompute::Cache` and `recompute::BlobStore`, so one object serves
/// both roles and a tessellated mesh reaches the shared disk tier exactly like a cooked shape. The
/// memory configuration needs two objects instead, `MemoryCache` and `MemoryBlobStore`. Each caller
/// that wires this by hand has to know that, and one of the two got a different answer: the shells
/// never had a disk tier at all, because `Controller` hard-coded the memory pair.
///
/// Choosing between them is now one constructor argument rather than a detail each application
/// rediscovers.
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
    /// In-memory tiers only: results live as long as this object. What a test wants, and what a
    /// shell with no cache directory configured gets.
    DocumentRuntime();

    /// With the on-disk DDC tier at `cacheDir`, which also backs the mesh store. Pass an empty
    /// path for assetlib's default location. The disk tier is what lets a result computed on one
    /// machine, or by CI, be served to another without recomputing.
    explicit DocumentRuntime(std::filesystem::path cacheDir);

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

    /// Whether computed results and meshes also persist to disk. Reported rather than inferred
    /// from the constructor used, so a caller can say so in a status line or a support bundle.
    [[nodiscard]] bool onDisk() const noexcept { return ddc_ != nullptr; }

private:
    recompute::FeatureRegistry registry_;

    /// Set when a disk tier was asked for, and then serves as BOTH cache and blob store.
    std::unique_ptr<recompute::DdcCache> ddc_;
    /// Set otherwise. Two objects, because the memory tiers are two types.
    std::unique_ptr<recompute::MemoryCache> memoryCache_;
    std::unique_ptr<recompute::MemoryBlobStore> memoryBlobs_;

    /// Whichever of the above is live. Non-owning: the owners are above, and which one is holding
    /// them is exactly the detail this class exists to stop spreading.
    recompute::Cache* cache_ = nullptr;
    recompute::BlobStore* blobs_ = nullptr;

    /// After the blob store, because it holds a reference to it.
    std::unique_ptr<render::MeshCache> meshes_;
};

}  // namespace cad::runtime
