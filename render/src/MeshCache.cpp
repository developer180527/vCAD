#include "cad/render/Tessellate.h"
#include <vector>
#include <unordered_set>
#include <thread>
#include <atomic>

#include <cstring>
#include "cad/kernel/Guard.h"
#include "cad/naming/ElementMap.h"

#include <sstream>

namespace cad::render {
namespace {

using kernel::Error;
using kernel::ErrorCode;

/// Bumped whenever the encoding below changes. Folded into the cache key, so old blobs become
/// unreachable rather than misread — the same rule as the feature-output format (ADR 0004).
constexpr std::uint32_t kMeshBlobVersion = 1;

void putU32(std::string& out, std::uint32_t v) {
    // Little-endian explicitly: the shared DDC tier is read by other machines.
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((v >> (i * 8)) & 0xFF));
}

bool getU32(const std::string& in, std::size_t& at, std::uint32_t& out) {
    if (at + 4 > in.size()) return false;
    out = 0;
    for (int i = 0; i < 4; ++i) {
        out |= static_cast<std::uint32_t>(static_cast<unsigned char>(in[at + std::size_t(i)]))
               << (i * 8);
    }
    at += 4;
    return true;
}

template <class T>
void putPod(std::string& out, const std::vector<T>& v) {
    putU32(out, static_cast<std::uint32_t>(v.size()));
    if (!v.empty()) {
        out.append(reinterpret_cast<const char*>(v.data()), v.size() * sizeof(T));
    }
}

template <class T>
bool getPod(const std::string& in, std::size_t& at, std::vector<T>& v) {
    std::uint32_t n = 0;
    if (!getU32(in, at, n)) return false;
    const std::size_t bytes = static_cast<std::size_t>(n) * sizeof(T);
    if (at + bytes > in.size()) return false;
    v.resize(n);
    if (n != 0) std::memcpy(v.data(), in.data() + at, bytes);
    at += bytes;
    return true;
}

std::string encode(const RenderMesh& m) {
    std::string out;
    // Reserve up front: a large mesh is tens of MB and repeated reallocation during encode is
    // a measurable part of a cache store.
    out.reserve(m.vertices.size() * sizeof(CadVertex) + m.indices.size() * 4
                + m.edgeVertices.size() * 4 + 1024);
    putU32(out, kMeshBlobVersion);

    // CadVertex, FaceRange and EdgeRange are POD and tightly packed (static_assert in the
    // header keeps CadVertex that way), so a bulk copy is safe and is what makes storing a
    // 20M-triangle assembly practical.
    putPod(out, m.vertices);
    putPod(out, m.indices);
    putPod(out, m.faces);
    putPod(out, m.edgeVertices);
    putPod(out, m.edges);

    // Element names go as text — they must round-trip through ElementName::parse, which is
    // the persisted form everywhere else too.
    putU32(out, static_cast<std::uint32_t>(m.elements.size()));
    for (const auto& n : m.elements) {
        const std::string text = n.toString();
        putU32(out, static_cast<std::uint32_t>(text.size()));
        out.append(text);
    }

    out.append(reinterpret_cast<const char*>(m.bounds.min), sizeof(m.bounds.min));
    out.append(reinterpret_cast<const char*>(m.bounds.max), sizeof(m.bounds.max));
    out.append(reinterpret_cast<const char*>(m.contentHash.lanes), sizeof(m.contentHash.lanes));
    return out;
}

/// Returns null on ANY problem. A blob we cannot read is a miss, never an error: a cache is
/// an optimisation and must not be able to fail a render.
std::shared_ptr<RenderMesh> decode(const std::string& in) {
    std::size_t at = 0;
    std::uint32_t version = 0;
    if (!getU32(in, at, version) || version != kMeshBlobVersion) return nullptr;

    auto m = std::make_shared<RenderMesh>();
    if (!getPod(in, at, m->vertices)) return nullptr;
    if (!getPod(in, at, m->indices)) return nullptr;
    if (!getPod(in, at, m->faces)) return nullptr;
    if (!getPod(in, at, m->edgeVertices)) return nullptr;
    if (!getPod(in, at, m->edges)) return nullptr;

    std::uint32_t count = 0;
    if (!getU32(in, at, count)) return nullptr;
    m->elements.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t len = 0;
        if (!getU32(in, at, len) || at + len > in.size()) return nullptr;
        m->elements.push_back(naming::ElementName::parse(in.substr(at, len)));
        at += len;
    }

    const std::size_t tail = sizeof(m->bounds.min) + sizeof(m->bounds.max)
                             + sizeof(m->contentHash.lanes);
    if (at + tail > in.size()) return nullptr;
    std::memcpy(m->bounds.min, in.data() + at, sizeof(m->bounds.min));
    at += sizeof(m->bounds.min);
    std::memcpy(m->bounds.max, in.data() + at, sizeof(m->bounds.max));
    at += sizeof(m->bounds.max);
    std::memcpy(m->contentHash.lanes, in.data() + at, sizeof(m->contentHash.lanes));

    // A mesh whose indices point outside its vertex array would fault the GPU driver, which
    // is a far worse outcome than a cache miss. Validate before trusting.
    for (std::uint32_t idx : m->indices) {
        if (idx >= m->vertices.size()) return nullptr;
    }
    for (const auto& f : m->faces) {
        if (static_cast<std::size_t>(f.indexOffset) + f.indexCount > m->indices.size()) {
            return nullptr;
        }
        if (f.element >= m->elements.size()) return nullptr;
    }
    for (const auto& e : m->edges) {
        if (static_cast<std::size_t>(e.vertexOffset) + e.vertexCount
            > m->edgeVertices.size() / 3) {
            return nullptr;
        }
        if (e.element >= m->elements.size()) return nullptr;
    }
    return m;
}

std::uint64_t cacheKey(const document::Output& output, const TessellationSettings& s) {
    // fold64, not lanes[0]. contentHash puts names in lane 0 and geometry in lanes 1-3, so
    // keying on lane 0 would make two same-named parts of different sizes share a mesh.
    const std::uint64_t shape = naming::contentHash(output.shape, output.map).fold64();
    // Distinct namespace from cooked feature output, and the blob version participates so a
    // format change cannot resurrect an unreadable blob.
    return shape ^ s.digest() ^ (0x6d657368ULL * (kMeshBlobVersion + 1));
}

}  // namespace

MeshCache::MeshCache(recompute::BlobStore& blobs) : blobs_(blobs) {}

std::size_t MeshCache::warm(std::span<const document::Output* const> outputs,
                            const TessellationSettings& settings) {
    // Phase 1, THIS thread: which keys are missing, deduplicated. Two placements of the same part
    // share a key, and tessellating it twice in parallel would be worse than doing it once here.
    struct Job {
        std::uint64_t key = 0;
        const document::Output* output = nullptr;
    };
    std::vector<Job> jobs;
    std::unordered_set<std::uint64_t> queued;
    for (const document::Output* output : outputs) {
        if (output == nullptr) continue;
        const std::uint64_t key = cacheKey(*output, settings);
        if (live_.find(key) != live_.end()) continue;
        if (!queued.insert(key).second) continue;
        jobs.push_back(Job{key, output});
    }
    if (jobs.empty()) return 0;

    // Phase 2, WORKERS: pure tessellation into a preallocated slot each. No shared mutable state,
    // no allocation of the vector itself, and therefore no lock on the hot path.
    std::vector<RenderMeshPtr> built(jobs.size());
    const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
    const std::size_t workers = std::min<std::size_t>(cores, jobs.size());

    std::atomic<std::size_t> next{0};
    const auto run = [&] {
        for (;;) {
            const std::size_t i = next.fetch_add(1, std::memory_order_relaxed);
            if (i >= jobs.size()) return;
            // A failure leaves the slot null. One unrenderable part must not take the assembly
            // with it — the same rule the serial path follows.
            if (auto mesh = tessellate(*jobs[i].output, settings)) built[i] = mesh.value();
        }
    };

    std::vector<std::thread> pool;
    pool.reserve(workers - 1);
    for (std::size_t w = 1; w < workers; ++w) pool.emplace_back(run);
    run();   // this thread takes a share rather than waiting
    for (std::thread& t : pool) t.join();

    // Phase 3, THIS thread again: publish. The cache has been single-threaded throughout.
    std::size_t made = 0;
    for (std::size_t i = 0; i < jobs.size(); ++i) {
        if (!built[i]) continue;
        live_.emplace(jobs[i].key, built[i]);
        blobs_.put(jobs[i].key, encode(*built[i]));
        ++made;
    }
    return made;
}

kernel::Result<RenderMeshPtr> MeshCache::get(const document::Output& output,
                                             const TessellationSettings& settings) {
    const std::uint64_t key = cacheKey(output, settings);

    // L0: live meshes, no decode at all. Panning and orbiting must never deserialise, and at
    // assembly scale this is also what makes dedupe free — the 50,000th identical bolt is a
    // hash lookup (ADR 0007 scale amendment).
    if (const auto it = live_.find(key); it != live_.end()) {
        ++hits_;
        return it->second;
    }

    // L1: the DDC. A hit here means another session — or another machine, via the shared tier
    // — already tessellated this exact geometry at these exact settings.
    std::string bytes;
    if (blobs_.get(key, bytes)) {
        if (auto decoded = decode(bytes)) {
            ++hits_;
            RenderMeshPtr shared = std::move(decoded);
            live_.emplace(key, shared);
            return shared;
        }
        // Unreadable: fall through and re-tessellate rather than fail.
    }

    ++misses_;
    auto built = tessellate(output, settings);
    if (!built) return built.error();

    RenderMeshPtr mesh = built.value();
    live_.emplace(key, mesh);

    blobs_.put(key, encode(*mesh));
    return mesh;
}

}  // namespace cad::render
