#include "cad/render/NullBackend.h"

namespace cad::render {
namespace {

/// Content hash plus a kind tag. The tag matters: a mesh's vertex and index buffers share one
/// content hash, and without it the second upload would be silently deduped against the first
/// and hand back a buffer of the wrong thing.
std::string internKey(const kernel::ShapeHash& h, int kind) {
    std::string key = h.hex();
    key.push_back(static_cast<char>('0' + kind));
    return key;
}

}  // namespace

BufferId NullGpuResources::intern(const kernel::ShapeHash& hash, int kind, std::uint64_t bytes) {
    const std::string key = internKey(hash, kind);
    if (const auto it = byContent_.find(key); it != byContent_.end()) {
        ++deduped_;
        return it->second;
    }
    const BufferId id{next_++};
    byContent_.emplace(key, id);
    sizes_.emplace(static_cast<std::uint64_t>(id), bytes);
    residentBytes_ += bytes;
    ++uploads_;
    return id;
}

BufferId NullGpuResources::uploadVertices(const kernel::ShapeHash& hash,
                                          std::span<const CadVertex> v) {
    if (v.empty()) return BufferId::None;
    return intern(hash, 0, v.size() * sizeof(CadVertex));
}

BufferId NullGpuResources::uploadIndices(const kernel::ShapeHash& hash,
                                         std::span<const std::uint32_t> i) {
    if (i.empty()) return BufferId::None;
    return intern(hash, 1, i.size() * sizeof(std::uint32_t));
}

BufferId NullGpuResources::uploadEdgeVertices(const kernel::ShapeHash& hash,
                                              std::span<const float> f) {
    if (f.empty()) return BufferId::None;
    return intern(hash, 2, f.size() * sizeof(float));
}

BufferId NullGpuResources::uploadInstances(std::uint64_t key, std::uint64_t revision,
                                           std::span<const Instance> instances) {
    if (instances.empty()) return BufferId::None;
    const std::uint64_t bytes = instances.size() * sizeof(Instance);

    auto& buffer = instanceBuffers_[key];
    if (buffer.id != BufferId::None && buffer.revision == revision) {
        // Same data. Skipping is not an optimisation here, it is the contract — an orbit must
        // not re-send instance data, and that is only observable through this counter.
        ++instanceSkips_;
        return buffer.id;
    }
    if (buffer.id == BufferId::None) {
        buffer.id = BufferId{next_++};
        sizes_.emplace(static_cast<std::uint64_t>(buffer.id), 0);
    }
    // Reuse the handle across revisions: the buffer is persistent, so an edit resizes it rather
    // than producing a new one, and nothing above the seam has to re-bind.
    residentBytes_ -= buffer.bytes;
    residentBytes_ += bytes;
    sizes_[static_cast<std::uint64_t>(buffer.id)] = bytes;
    buffer.bytes = bytes;
    buffer.revision = revision;
    ++instanceUploads_;
    return buffer.id;
}

void NullGpuResources::release(BufferId id) {
    if (id == BufferId::None) return;
    const auto it = sizes_.find(static_cast<std::uint64_t>(id));
    if (it == sizes_.end()) return;   // release of an unknown handle is a no-op, not a fault
    residentBytes_ -= it->second;
    sizes_.erase(it);
    for (auto i = byContent_.begin(); i != byContent_.end(); ++i) {
        if (i->second == id) {
            byContent_.erase(i);
            break;
        }
    }
    for (auto i = instanceBuffers_.begin(); i != instanceBuffers_.end(); ++i) {
        if (i->second.id == id) {
            instanceBuffers_.erase(i);
            break;
        }
    }
}

void NullFrameSink::submit(const SceneFrame& f) {
    ++frames_;
    stats_ = Stats{};
    recorded_ = Recorded{};

    // One draw call PER RANGE, matching what a real backend must do: a range is a contiguous
    // run of the persistent instance buffer, and a backend cannot submit two disjoint runs in
    // one call. An entirely visible batch collapses to one range, so this stays at one call per
    // batch until culling actually fragments it.
    for (const auto& b : f.batches) {
        if (b.indexCount == 0 || b.instances == BufferId::None) continue;
        std::uint32_t visible = 0;
        for (const DrawRange& r : b.ranges) {
            if (r.instanceCount == 0) continue;
            ++stats_.drawCalls;
            visible += r.instanceCount;
        }
        if (visible == 0) continue;
        stats_.instancesRequested += b.instanceCount;
        stats_.instances += visible;
        stats_.triangles += (b.indexCount / 3) * visible;
        recorded_.vertexBuffers.push_back(b.vertices);
        recorded_.instanceCounts.push_back(visible);
    }
    for (const auto& e : f.edgeBatches) {
        if (e.vertexCount == 0 || e.instances == BufferId::None) continue;
        std::uint32_t visible = 0;
        for (const DrawRange& r : e.ranges) {
            if (r.instanceCount == 0) continue;
            ++stats_.drawCalls;
            visible += r.instanceCount;
        }
        if (visible == 0) continue;
        stats_.lines += (e.vertexCount - 1) * visible;
        ++recorded_.edgeBatches;
    }
    for (const Highlight h : f.highlights) {
        if (h != Highlight::None) ++recorded_.highlighted;
    }
    recorded_.elementCount = f.elementCount;
    recorded_.orthographic = f.camera.orthographic;
}

IPicker::Hit NullPicker::pick(const SceneFrame&, std::uint32_t, std::uint32_t) {
    ++picks_;
    return next_;
}

void NullPicker::pickRect(const SceneFrame&, std::uint32_t, std::uint32_t, std::uint32_t,
                          std::uint32_t, std::vector<std::uint32_t>& out) {
    ++picks_;
    out.clear();
    if (next_.valid) out.push_back(next_.element);
}

NullBackend::NullBackend() {
    backend_.resources = &resources;
    backend_.frames = &frames;
    backend_.picker = &picker;
    backend_.name = "null";
}

}  // namespace cad::render
