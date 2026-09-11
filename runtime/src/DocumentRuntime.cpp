#include "cad/runtime/DocumentRuntime.h"

#include "cad/features/Builtins.h"

#include <utility>

namespace cad::runtime {

DocumentRuntime::DocumentRuntime() : registry_(features::builtins()) {
    memoryCache_ = std::make_unique<recompute::MemoryCache>();
    memoryBlobs_ = std::make_unique<recompute::MemoryBlobStore>();
    cache_ = memoryCache_.get();
    blobs_ = memoryBlobs_.get();
    meshes_ = std::make_unique<render::MeshCache>(*blobs_);
}

DocumentRuntime::DocumentRuntime(std::filesystem::path cacheDir)
    : registry_(features::builtins()) {
    // ONE object in both roles. See the header: the DDC is a Cache and a BlobStore, so a mesh
    // reaches the shared tier the same way a cooked shape does. Splitting them here would give the
    // meshes a memory store and quietly halve what the disk tier is for.
    ddc_ = std::make_unique<recompute::DdcCache>(std::move(cacheDir));
    cache_ = ddc_.get();
    blobs_ = ddc_.get();
    meshes_ = std::make_unique<render::MeshCache>(*blobs_);
}

// Out of line, because the members are unique_ptrs to types the header only forward-needs through
// their own headers -- and because the ORDER matters: the mesh cache holds a reference to the blob
// store, so it must go first. Declaration order gives that today; saying it here means a future
// reorder of the members cannot silently change it.
DocumentRuntime::~DocumentRuntime() {
    meshes_.reset();
    blobs_ = nullptr;
    cache_ = nullptr;
    memoryBlobs_.reset();
    memoryCache_.reset();
    ddc_.reset();
}

}  // namespace cad::runtime
