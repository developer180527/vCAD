#include "cad/runtime/DocumentRuntime.h"

#include "cad/features/Builtins.h"

#include <utility>

namespace cad::runtime {

DocumentRuntime::DocumentRuntime(std::filesystem::path cacheDir)
    : registry_(features::builtins()) {
    auto l0 = std::make_unique<recompute::MemoryCache>();

    if (cacheDir.empty()) {
        cache_ = std::move(l0);
        memoryBlobs_ = std::make_unique<recompute::MemoryBlobStore>();
        blobs_ = memoryBlobs_.get();
    } else {
        // TIERED, not the DDC alone. L0 answers the repeated lookups an interactive edit depends
        // on; L1 is what survives a restart and is shared between machines. Handing the DDC over
        // as the whole cache would send every lookup to disk -- which is what the first version of
        // this class did, and it would have silently removed a tier the ABI session already had.
        auto ddc = std::make_unique<recompute::DdcCache>(std::move(cacheDir));
        ddc_ = ddc.get();
        // The DDC serves the blob role too, so a tessellated mesh reaches the shared tier exactly
        // as a cooked shape does. The TieredCache takes ownership; `ddc_` stays non-owning.
        blobs_ = ddc_;
        cache_ = std::make_unique<recompute::TieredCache>(std::move(l0), std::move(ddc));
    }

    meshes_ = std::make_unique<render::MeshCache>(*blobs_);
}

// Out of line, and explicit about ORDER: the mesh cache holds a reference to the blob store, and on
// the disk path the blob store is owned by the cache. Declaration order gives the right answer
// today; saying it here means a future reorder of the members cannot silently change it.
DocumentRuntime::~DocumentRuntime() {
    meshes_.reset();
    blobs_ = nullptr;
    ddc_ = nullptr;      // owned by cache_ on the disk path; never freed here
    cache_.reset();
    memoryBlobs_.reset();
}

}  // namespace cad::runtime
