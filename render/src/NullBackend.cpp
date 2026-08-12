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
}

void NullFrameSink::submit(const SceneFrame& f) {
    ++frames_;
    stats_ = Stats{};
    recorded_ = Recorded{};

    for (const auto& b : f.batches) {
        if (b.indexCount == 0 || b.instances.empty()) continue;
        ++stats_.drawCalls;
        stats_.instances += static_cast<std::uint32_t>(b.instances.size());
        stats_.triangles +=
            static_cast<std::uint32_t>((b.indexCount / 3) * b.instances.size());
        recorded_.vertexBuffers.push_back(b.vertices);
        recorded_.instanceCounts.push_back(static_cast<std::uint32_t>(b.instances.size()));
    }
    for (const auto& e : f.edgeBatches) {
        if (e.vertexCount == 0 || e.instances.empty()) continue;
        ++stats_.drawCalls;
        stats_.lines += static_cast<std::uint32_t>((e.vertexCount - 1) * e.instances.size());
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
