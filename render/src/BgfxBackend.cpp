#include "cad/render/BgfxBackend.h"

#include "cad/log/Log.h"

#include "cad/render/MetalSurface.h"

#include <bgfx/bgfx.h>
#include <bgfx/platform.h>
#include <bx/math.h>

#include <algorithm>
#include <cstdio>
#include <type_traits>
#include <cstring>
#include <fstream>
#include <mutex>
#include <unordered_map>
#include <span>
#include <vector>

namespace cad::render {
namespace {

/// Width of the per-element highlight lookup, in texels.
///
/// Rows rather than one long texture because the maximum 2D texture dimension is 16384 on plenty of
/// hardware and a large assembly has more elements than that. 4096 keeps the row count small while
/// staying far inside every limit.
constexpr std::uint16_t kHighlightLutWidth = 4096;

}  // namespace

namespace {

using kernel::Error;
using kernel::ErrorCode;

constexpr bgfx::ViewId kViewShaded = 0;
constexpr bgfx::ViewId kViewPick = 1;
/// Blits are submitted on their own view so they are ordered after both render passes. A blit on
/// a view that also draws is not guaranteed to see that view's output.
constexpr bgfx::ViewId kViewBlit = 2;

/// bgfx has no integer vertex attributes, so the element index rides as four unnormalised
/// uint8 channels and is reassembled in the vertex shader. Standard bgfx idiom.
bgfx::VertexLayout& cadVertexLayout() {
    static bgfx::VertexLayout layout = [] {
        bgfx::VertexLayout l;
        l.begin()
            .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
            .add(bgfx::Attrib::Normal, 3, bgfx::AttribType::Float)
            .add(bgfx::Attrib::Color1, 4, bgfx::AttribType::Uint8, /*normalized*/ false,
                 /*asInt*/ false)
            .end();
        return l;
    }();
    return layout;
}

/// Layout for a PERSISTENT instance buffer.
///
/// bgfx delivers instance data as `i_data0..N`, and when the data comes from a vertex buffer
/// rather than the transient allocator it decides how many slots that is from the buffer's
/// stride. TexCoord7 downwards is bgfx's own convention for those slots; four Float4s give the
/// 64-byte stride `Instance` asserts.
bgfx::VertexLayout& instanceLayout() {
    static bgfx::VertexLayout layout = [] {
        bgfx::VertexLayout l;
        l.begin()
            .add(bgfx::Attrib::TexCoord7, 4, bgfx::AttribType::Float)
            .add(bgfx::Attrib::TexCoord6, 4, bgfx::AttribType::Float)
            .add(bgfx::Attrib::TexCoord5, 4, bgfx::AttribType::Float)
            .add(bgfx::Attrib::TexCoord4, 4, bgfx::AttribType::Float)
            .end();
        return l;
    }();
    return layout;
}

bgfx::VertexLayout& edgeVertexLayout() {
    static bgfx::VertexLayout layout = [] {
        bgfx::VertexLayout l;
        l.begin().add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float).end();
        return l;
    }();
    return layout;
}

/// Set by the shell to the directory beside its executable. Empty until then.
std::string& shaderDirectory() {
    static std::string dir;
    return dir;
}

/// Shader binaries live next to the executable, produced by shaderc at build time. Loaded
/// rather than embedded so a shader edit does not require relinking the whole application.
///
/// Order of preference, first that is set:
///   1. $CAD_SHADER_DIR                 -- explicit always wins
///   2. whatever the shell registered   -- beside the executable, normally
///   3. "shaders", relative to the CWD  -- the spikes, which are run from the build directory
///
/// Step 2 exists because this used to be steps 1 and 3 only, while the comment above claimed
/// shaders were loaded from beside the executable. They were loaded from beside the WORKING
/// DIRECTORY, so launching the shell from anywhere else found no shaders and drew nothing. Same
/// mistake as the log file, which was resolved against the platform's app-data directory for an
/// application that ships as a bare executable.
std::string shaderPath(const std::string& name) {
    const char* env = std::getenv("CAD_SHADER_DIR");
    const std::string base = (env != nullptr && *env != '\0') ? std::string(env)
                             : !shaderDirectory().empty() ? shaderDirectory()
                                                          : std::string("shaders");
    std::string profile;
    switch (bgfx::getRendererType()) {
        case bgfx::RendererType::Metal:     profile = "metal"; break;
        case bgfx::RendererType::Direct3D11:
        case bgfx::RendererType::Direct3D12: profile = "dx11"; break;
        case bgfx::RendererType::Vulkan:    profile = "spirv"; break;
        case bgfx::RendererType::OpenGL:    profile = "glsl"; break;
        default:                            profile = "noop"; break;
    }
    return base + "/" + profile + "/" + name + ".bin";
}

bgfx::ShaderHandle loadShader(const std::string& name) {
    const std::string path = shaderPath(name);
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return BGFX_INVALID_HANDLE;
    const std::streamsize size = in.tellg();
    if (size <= 0) return BGFX_INVALID_HANDLE;
    in.seekg(0);

    const bgfx::Memory* mem = bgfx::alloc(static_cast<std::uint32_t>(size) + 1);
    in.read(reinterpret_cast<char*>(mem->data), size);
    mem->data[size] = '\0';   // bgfx wants shader blobs NUL-terminated
    bgfx::ShaderHandle h = bgfx::createShader(mem);
    if (bgfx::isValid(h)) bgfx::setName(h, name.c_str());
    return h;
}

/// bgfx callback, implemented for one reason: `fatal`. Without it a fatal error inside bgfx is
/// invisible, which is how "nothing draws and nothing is logged" happened repeatedly.
///
/// It deliberately does NOT implement frame capture. `bgfx::requestScreenShot` looks like the
/// documented way to read a framebuffer back, and it is — for the BACKBUFFER only. Metal's
/// implementation does `BX_UNUSED(_handle)` and captures `m_screenshotTarget`, returning silently
/// when that is null, which it always is when the backbuffer is 0x0 as headless mode requires.
/// So an offscreen capture request produced no callback, no pixels and no error. Capture goes
/// through blit + readTexture instead; see BgfxBackend::captureFrame.
class DiagnosticCallback final : public bgfx::CallbackI {
public:
    void fatal(const char* filePath, std::uint16_t line, bgfx::Fatal::Enum code,
               const char* str) override {
        std::fprintf(stderr, "bgfx fatal [%d] %s:%u: %s\n", int(code), filePath, line, str);
    }
    void traceVargs(const char*, std::uint16_t, const char*, va_list) override {}
    void profilerBegin(const char*, std::uint32_t, const char*, std::uint16_t) override {}
    void profilerBeginLiteral(const char*, std::uint32_t, const char*, std::uint16_t) override {}
    void profilerEnd() override {}
    std::uint32_t cacheReadSize(std::uint64_t) override { return 0; }
    bool cacheRead(std::uint64_t, void*, std::uint32_t) override { return false; }
    void cacheWrite(std::uint64_t, const void*, std::uint32_t) override {}
    void captureBegin(std::uint32_t, std::uint32_t, std::uint32_t, bgfx::TextureFormat::Enum,
                      bool) override {}
    void captureEnd() override {}
    void captureFrame(const void*, std::uint32_t) override {}

    /// Unused — see the class comment. Left empty rather than wired to anything, so it cannot
    /// look like a working capture path.
    void screenShot(const char*, std::uint32_t, std::uint32_t, std::uint32_t, const void*,
                    std::uint32_t, bool) override {}
};

}  // namespace

// ── resources ───────────────────────────────────────────────────────────────────────────

class BgfxResources final : public IGpuResources {
public:
    BufferId uploadVertices(const kernel::ShapeHash& hash,
                            std::span<const CadVertex> v) override {
        if (v.empty()) return BufferId::None;
        // Into the SHARED arena, not a buffer of its own. One buffer per mesh exhausted bgfx's
        // fixed pool of 4096 handles at about 2048 unique parts and then drew nothing.
        return intern(hash, 0, v.size() * sizeof(CadVertex), [&] {
            return appendToArena(surfaceArena_, v.data(), static_cast<std::uint32_t>(v.size()),
                                 v.size_bytes(), &cadVertexLayout(), Kind::Vertex);
        });
    }

    BufferId uploadIndices(const kernel::ShapeHash& hash,
                           std::span<const std::uint32_t> i) override {
        if (i.empty()) return BufferId::None;
        return intern(hash, 1, i.size_bytes(), [&] {
            return appendToArena(indexArena_, i.data(), static_cast<std::uint32_t>(i.size()),
                                 i.size_bytes(), nullptr, Kind::Index);
        });
    }

    BufferId uploadEdgeVertices(const kernel::ShapeHash& hash,
                                std::span<const float> f) override {
        if (f.empty()) return BufferId::None;
        // Three floats per edge vertex, so the element count is a third of the span.
        return intern(hash, 2, f.size_bytes(), [&] {
            return appendToArena(edgeArena_, f.data(), static_cast<std::uint32_t>(f.size() / 3),
                                 f.size_bytes(), &edgeVertexLayout(), Kind::Edge);
        });
    }

    /// Persistent instance buffer, updated in place rather than refilled per frame.
    ///
    /// A DYNAMIC vertex buffer, not the transient instance allocator: transient has a fixed
    /// 6 MB per-frame budget and TRUNCATES SILENTLY past it, which drew 98% of a 100k-part
    /// assembly at a convincing frame rate. Dynamic buffers have no such cap and survive across
    /// frames, so an orbit re-sends nothing.
    BufferId uploadInstances(std::uint64_t key, std::uint64_t revision,
                             std::span<const Instance> instances) override {
        if (instances.empty()) return BufferId::None;
        auto& slot = instanceBuffers_[key];

        if (slot.id != BufferId::None && slot.revision == revision) return slot.id;

        const bgfx::Memory* mem =
            bgfx::copy(instances.data(), static_cast<std::uint32_t>(instances.size_bytes()));

        if (slot.id == BufferId::None) {
            // ALLOW_RESIZE: without it bgfx silently TRIMS an update larger than the original
            // allocation — the same class of quiet truncation this whole change is here to kill.
            const auto h = bgfx::createDynamicVertexBuffer(mem, instanceLayout(),
                                                          BGFX_BUFFER_ALLOW_RESIZE);
            if (!bgfx::isValid(h)) return BufferId::None;
            slot.id = BufferId{next_++};
            entries_.emplace(static_cast<std::uint64_t>(slot.id),
                             Entry{h.idx, Kind::Instance, instances.size_bytes()});
            resident_ += instances.size_bytes();
        } else {
            Entry& entry = entries_.at(static_cast<std::uint64_t>(slot.id));
            bgfx::update(bgfx::DynamicVertexBufferHandle{entry.idx}, 0, mem);
            resident_ -= entry.bytes;
            entry.bytes = instances.size_bytes();
            resident_ += entry.bytes;
        }
        slot.revision = revision;
        return slot.id;
    }

    BufferId uploadDynamicVertices(std::uint64_t key, std::uint64_t revision,
                                   std::span<const CadVertex> v) override {
        if (v.empty()) return BufferId::None;
        return uploadDynamic(dynamicVertexBuffers_, key, revision, v.data(), v.size_bytes(),
                             cadVertexLayout(), Kind::DynamicVertex);
    }

    BufferId uploadDynamicIndices(std::uint64_t key, std::uint64_t revision,
                                  std::span<const std::uint32_t> i) override {
        if (i.empty()) return BufferId::None;
        auto& slot = dynamicIndexBuffers_[key];
        if (slot.id != BufferId::None && slot.revision == revision) return slot.id;
        const bgfx::Memory* mem = bgfx::copy(i.data(), static_cast<std::uint32_t>(i.size_bytes()));
        if (slot.id == BufferId::None) {
            // INDEX32 and ALLOW_RESIZE, for the same reasons the static index path gives: 16-bit
            // truncation renders convincing garbage, and a growing sketch must not be trimmed.
            const auto h = bgfx::createDynamicIndexBuffer(
                mem, BGFX_BUFFER_INDEX32 | BGFX_BUFFER_ALLOW_RESIZE);
            if (!bgfx::isValid(h)) return BufferId::None;
            slot.id = BufferId{next_++};
            entries_.emplace(static_cast<std::uint64_t>(slot.id),
                             Entry{h.idx, Kind::DynamicIndex, i.size_bytes()});
            resident_ += i.size_bytes();
        } else {
            Entry& entry = entries_.at(static_cast<std::uint64_t>(slot.id));
            bgfx::update(bgfx::DynamicIndexBufferHandle{entry.idx}, 0, mem);
            resident_ -= entry.bytes;
            entry.bytes = i.size_bytes();
            resident_ += entry.bytes;
        }
        slot.revision = revision;
        return slot.id;
    }

    BufferId uploadDynamicEdgeVertices(std::uint64_t key, std::uint64_t revision,
                                       std::span<const float> f) override {
        if (f.empty()) return BufferId::None;
        auto& slot = dynamicEdgeBuffers_[key];
        if (slot.id != BufferId::None && slot.revision == revision) return slot.id;

        const bgfx::Memory* mem = bgfx::copy(f.data(), static_cast<std::uint32_t>(f.size_bytes()));
        if (slot.id == BufferId::None) {
            // ALLOW_RESIZE for the same reason the instance buffer has it: a sketch grows as it is
            // drawn, and without it bgfx TRIMS the update to the first allocation's size, so the
            // lines drawn after some threshold would silently stop appearing.
            const auto h = bgfx::createDynamicVertexBuffer(mem, edgeVertexLayout(),
                                                          BGFX_BUFFER_ALLOW_RESIZE);
            if (!bgfx::isValid(h)) return BufferId::None;
            slot.id = BufferId{next_++};
            entries_.emplace(static_cast<std::uint64_t>(slot.id),
                             Entry{h.idx, Kind::DynamicEdge, f.size_bytes()});
            resident_ += f.size_bytes();
        } else {
            Entry& entry = entries_.at(static_cast<std::uint64_t>(slot.id));
            bgfx::update(bgfx::DynamicVertexBufferHandle{entry.idx}, 0, mem);
            resident_ -= entry.bytes;
            entry.bytes = f.size_bytes();
            resident_ += entry.bytes;
        }
        slot.revision = revision;
        return slot.id;
    }

    void release(BufferId id) override {
        const auto it = entries_.find(static_cast<std::uint64_t>(id));
        if (it == entries_.end()) return;
        if (it->second.arena) {
            // A suballocation owns nothing. Freeing the arena here would destroy every other mesh
            // sharing it; reclaiming the space needs compaction, which is not built — see
            // SCALE_REVIEW.md. An editing session therefore grows its arenas and never shrinks
            // them, which is bounded by the document rather than by how long the session ran.
            resident_ -= it->second.bytes;
            entries_.erase(it);
            // The content->id mapping goes WITH it. Leaving it behind means the next upload of the
            // same mesh gets a cache hit on an id whose entry no longer exists, `find` returns
            // null, and every batch using that mesh is skipped — the scene rebuilds and draws
            // nothing. Caught by the spike's single-instance baseline stage at 20k parts, where
            // the main scene rendered correctly and the rebuild after it did not.
            for (auto k = byContent_.begin(); k != byContent_.end(); ++k) {
                if (k->second == id) {
                    byContent_.erase(k);
                    break;
                }
            }
            return;
        }
        switch (it->second.kind) {
            case Kind::Vertex:
            case Kind::Edge:
                bgfx::destroy(bgfx::VertexBufferHandle{it->second.idx});
                break;
            case Kind::Index:
                bgfx::destroy(bgfx::IndexBufferHandle{it->second.idx});
                break;
            case Kind::Instance:
            case Kind::DynamicEdge:
            case Kind::DynamicVertex:
                bgfx::destroy(bgfx::DynamicVertexBufferHandle{it->second.idx});
                break;
            case Kind::DynamicIndex:
                bgfx::destroy(bgfx::DynamicIndexBufferHandle{it->second.idx});
                break;
        }
        resident_ -= it->second.bytes;
        for (auto k = byContent_.begin(); k != byContent_.end(); ++k) {
            if (k->second == id) { byContent_.erase(k); break; }
        }
        for (auto k = instanceBuffers_.begin(); k != instanceBuffers_.end(); ++k) {
            if (k->second.id == id) { instanceBuffers_.erase(k); break; }
        }
        for (auto k = dynamicEdgeBuffers_.begin(); k != dynamicEdgeBuffers_.end(); ++k) {
            if (k->second.id == id) { dynamicEdgeBuffers_.erase(k); break; }
        }
        entries_.erase(it);
    }

    [[nodiscard]] std::uint64_t residentBytes() const override { return resident_; }

    enum class Kind : std::uint8_t { Vertex, Index, Edge, Instance, DynamicEdge,
                                     DynamicVertex, DynamicIndex };
    struct Entry {
        std::uint16_t idx = bgfx::kInvalidHandle;
        Kind kind = Kind::Vertex;
        std::size_t bytes = 0;

        /// Where this mesh lives INSIDE a shared buffer, in elements (vertices or indices).
        ///
        /// A BufferId used to name a whole GPU buffer, one per mesh — which ran bgfx's fixed pool
        /// of 4096 handles dry at about 2048 unique parts and silently stopped drawing. It now
        /// names a SUBALLOCATION: many meshes share one large buffer and differ by offset. Nothing
        /// above the backend changed, because nothing above it ever looked inside a BufferId.
        std::uint32_t offset = 0;
        std::uint32_t count = 0;
        bool arena = false;   ///< false for the standalone buffers the dynamic paths still use
    };
    [[nodiscard]] const Entry* find(BufferId id) const {
        const auto it = entries_.find(static_cast<std::uint64_t>(id));
        return it == entries_.end() ? nullptr : &it->second;
    }

    /// Destroys every buffer. Without this bgfx prints "BGFX LEAK: VertexBufferHandle ..." at
    /// shutdown — which it does report, but only after the fact and only to stderr, so it is
    /// easy to ship. Called from BgfxBackend::shutdown before bgfx::shutdown.
    void releaseAll() {
        // The arenas own their chunks; the entries pointing into them do not.
        for (Arena* arena : {&surfaceArena_, &edgeArena_, &indexArena_}) {
            for (const Chunk& chunk : arena->chunks) {
                if (chunk.idx == bgfx::kInvalidHandle) continue;
                if (arena->index) {
                    bgfx::destroy(bgfx::DynamicIndexBufferHandle{chunk.idx});
                } else {
                    bgfx::destroy(bgfx::DynamicVertexBufferHandle{chunk.idx});
                }
            }
            *arena = Arena{};
        }

        for (const auto& [id, entry] : entries_) {
            if (entry.arena) continue;   // owned by the arena, destroyed above
            switch (entry.kind) {
                case Kind::Vertex:
                case Kind::Edge:
                    bgfx::destroy(bgfx::VertexBufferHandle{entry.idx});
                    break;
                case Kind::Index:
                    bgfx::destroy(bgfx::IndexBufferHandle{entry.idx});
                    break;
                case Kind::Instance:
                case Kind::DynamicEdge:
                case Kind::DynamicVertex:
                    bgfx::destroy(bgfx::DynamicVertexBufferHandle{entry.idx});
                    break;
                case Kind::DynamicIndex:
                    bgfx::destroy(bgfx::DynamicIndexBufferHandle{entry.idx});
                    break;
            }
        }
        entries_.clear();
        byContent_.clear();
        instanceBuffers_.clear();
        resident_ = 0;
    }

private:
template <class Make>
    BufferId intern(const kernel::ShapeHash& hash, int kind, std::size_t bytes, Make&& make) {
        // Kind is part of the key: a mesh's vertex and index buffers share one content hash,
        // and without the tag the second would dedupe onto the first and return the wrong
        // buffer entirely.
        std::string key = hash.hex();
        key.push_back(static_cast<char>('0' + kind));
        if (const auto it = byContent_.find(key); it != byContent_.end()) return it->second;

        const Entry entry = make();
        if (entry.idx == bgfx::kInvalidHandle) return BufferId::None;
        const BufferId id{next_++};
        byContent_.emplace(std::move(key), id);
        entries_.emplace(static_cast<std::uint64_t>(id), entry);
        resident_ += bytes;
        return id;
    }

    /// Instance buffers are keyed by BATCH, not by content hash: placement data is not shared
    /// between batches, and it changes on every edit.
    struct InstanceSlot {
        BufferId id = BufferId::None;
        std::uint64_t revision = 0;
    };

    std::unordered_map<std::string, BufferId> byContent_;
    std::unordered_map<std::uint64_t, Entry> entries_;
    /// The common body of the dynamic vertex uploads: same slot, same resize rule, different
    /// layout and kind. Written once because the three copies differed only in those two.
    BufferId uploadDynamic(std::unordered_map<std::uint64_t, InstanceSlot>& slots,
                           std::uint64_t key, std::uint64_t revision, const void* data,
                           std::size_t bytes, const bgfx::VertexLayout& layout, Kind kind) {
        auto& slot = slots[key];
        if (slot.id != BufferId::None && slot.revision == revision) return slot.id;
        const bgfx::Memory* mem = bgfx::copy(data, static_cast<std::uint32_t>(bytes));
        if (slot.id == BufferId::None) {
            const auto h = bgfx::createDynamicVertexBuffer(mem, layout, BGFX_BUFFER_ALLOW_RESIZE);
            if (!bgfx::isValid(h)) return BufferId::None;
            slot.id = BufferId{next_++};
            entries_.emplace(static_cast<std::uint64_t>(slot.id), Entry{h.idx, kind, bytes});
            resident_ += bytes;
        } else {
            Entry& entry = entries_.at(static_cast<std::uint64_t>(slot.id));
            bgfx::update(bgfx::DynamicVertexBufferHandle{entry.idx}, 0, mem);
            resident_ -= entry.bytes;
            entry.bytes = bytes;
            resident_ += entry.bytes;
        }
        slot.revision = revision;
        return slot.id;
    }

    /// Meshes packed into a small number of large buffers.
    ///
    /// One buffer per mesh exhausted bgfx's fixed pool of 4096 handles at about 2048 unique parts
    /// and then drew nothing at all. Packing many meshes into each buffer removes that ceiling and
    /// is the precondition for drawing several meshes per call at all.
    ///
    /// Growth is a CHAIN OF CHUNKS rather than a resize. bgfx's ALLOW_RESIZE did not grow these —
    /// every update past the initial size was truncated, which it reports and then carries on from,
    /// the exact silent-truncation failure this work exists to escape. Adding a chunk needs no
    /// re-upload and no CPU copy of geometry already on the GPU, and the handle count becomes
    /// total-size/chunk-size: a handful, not thousands.
    struct Chunk {
        std::uint16_t idx = bgfx::kInvalidHandle;
        std::uint32_t used = 0;
        std::uint32_t capacity = 0;
    };
    struct Arena {
        std::vector<Chunk> chunks;
        bool index = false;
    };
    Arena surfaceArena_;
    Arena edgeArena_;
    Arena indexArena_;

    /// Appends `count` elements, opening a new chunk when the last one cannot hold them.
    Entry appendToArena(Arena& arena, const void* data, std::uint32_t count, std::size_t bytes,
                        const bgfx::VertexLayout* layout, Kind kind) {
        if (count == 0) return Entry{};

        // A chunk large enough that a normal document needs one or two, and small enough that the
        // first one is not a surprise allocation on a machine with modest memory.
        constexpr std::uint32_t kChunkElements = 1u << 19;

        Chunk* chunk = arena.chunks.empty() ? nullptr : &arena.chunks.back();
        if (chunk == nullptr || chunk->used + count > chunk->capacity) {
            Chunk fresh;
            fresh.capacity = std::max(count, kChunkElements);
            if (layout != nullptr) {
                const auto h = bgfx::createDynamicVertexBuffer(fresh.capacity, *layout,
                                                               BGFX_BUFFER_COMPUTE_READ);
                if (!bgfx::isValid(h)) return Entry{};
                fresh.idx = h.idx;
            } else {
                const auto h = bgfx::createDynamicIndexBuffer(
                    fresh.capacity, BGFX_BUFFER_INDEX32 | BGFX_BUFFER_COMPUTE_READ);
                if (!bgfx::isValid(h)) return Entry{};
                fresh.idx = h.idx;
                arena.index = true;
            }
            arena.chunks.push_back(fresh);
            chunk = &arena.chunks.back();
        }

        const bgfx::Memory* mem = bgfx::copy(data, static_cast<std::uint32_t>(bytes));
        if (arena.index) {
            bgfx::update(bgfx::DynamicIndexBufferHandle{chunk->idx}, chunk->used, mem);
        } else {
            bgfx::update(bgfx::DynamicVertexBufferHandle{chunk->idx}, chunk->used, mem);
        }

        Entry entry;
        entry.idx = chunk->idx;
        entry.kind = kind;
        entry.bytes = bytes;
        entry.offset = chunk->used;
        entry.count = count;
        entry.arena = true;
        chunk->used += count;
        return entry;
    }

    std::unordered_map<std::uint64_t, InstanceSlot> instanceBuffers_;
    /// Same slot shape as the instance buffers, and keyed the same way: one buffer per owner for
    /// the life of an editing session rather than one per edit.
    std::unordered_map<std::uint64_t, InstanceSlot> dynamicEdgeBuffers_;
    std::unordered_map<std::uint64_t, InstanceSlot> dynamicVertexBuffers_;
    std::unordered_map<std::uint64_t, InstanceSlot> dynamicIndexBuffers_;
    std::uint64_t next_ = 1;
    std::uint64_t resident_ = 0;
};

// ── frame submission ────────────────────────────────────────────────────────────────────

struct BgfxBackend::Impl {
    BgfxConfig config;
    bool initialised = false;
    DiagnosticCallback callback;

    /// A CAMetalLayer we created because offscreen mode had no window. Null when the caller
    /// supplied its own surface, in which case releasing it is the caller's business.
    void* ownedSurface = nullptr;

    BgfxResources resources;

    bgfx::ProgramHandle shaded = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle edge = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle pick = BGFX_INVALID_HANDLE;

    bgfx::UniformHandle uShading = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle uHighlight = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle sHighlight = BGFX_INVALID_HANDLE;

    /// Per-element highlight state, as a texture the shader looks up by element slot.
    ///
    /// A texture rather than a uniform because `u_highlight` was a uniform and that is exactly why
    /// nothing was ever highlighted: a uniform is per DRAW CALL, and a draw call covers a whole mesh
    /// across every placement of it. Selection is per element. There was no value to put in the
    /// uniform that could mean "this face and not the other five", so it was set to zero strength
    /// permanently and the highlight table the scene had been maintaining went nowhere.
    ///
    /// R8, one byte per element slot, laid out in rows of `kHighlightLutWidth`. The shaded vertex
    /// shader already carries the absolute slot in `v_ids.x` for picking; this reuses it.
    bgfx::TextureHandle highlightLut = BGFX_INVALID_HANDLE;
    std::uint16_t highlightLutHeight = 0;
    /// The last bytes uploaded, so an idle redraw uploads nothing. Hover fires on every mouse-move.
    std::vector<std::uint8_t> highlightUploaded;

    /// Uploads `highlights` if they changed, and binds the lookup. Returns the LUT dimensions.
    void syncHighlights(std::span<const Highlight> highlights);
    bgfx::UniformHandle uEdgeParams = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle uEdgeColor = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle uSectionPlane = BGFX_INVALID_HANDLE;

    /// xyz = plane normal, w = offset. All zero means no section, which is also the value an
    /// unset uniform already holds — so nothing clips until something asks.
    float sectionPlane[4]{0.0f, 0.0f, 0.0f, 0.0f};

    // Offscreen / pick targets.
    //
    // Each render target has a matching readback texture. A render target cannot be read
    // directly: bgfx::readTexture requires BGFX_TEXTURE_READ_BACK, which BGFX_TEXTURE_RT does not
    // imply, and asking anyway asserts inside bgfx rather than returning an error. So the pattern
    // is always blit RT -> readback, then readTexture the readback.
    /// Creates the offscreen colour and pick targets at this size. Split out of initialise()
    /// so resize() can rebuild them: in offscreen mode the textures ARE the framebuffer, and a
    /// resize that only updated config.viewport left captureFrame reading back at the new
    /// dimensions from a texture still sized at attach time. Each row then started at the wrong
    /// offset and the image arrived skewed into diagonal streaks.
    void createTargets(std::uint16_t w, std::uint16_t h);
    void destroyTargets();

    bgfx::FrameBufferHandle colourFb = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle colourTarget = BGFX_INVALID_HANDLE;    ///< the RT the shaded pass writes
    bgfx::TextureHandle colourReadback = BGFX_INVALID_HANDLE;  ///< BLIT_DST|READ_BACK copy
    bgfx::FrameBufferHandle pickFb = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle pickTarget = BGFX_INVALID_HANDLE;    ///< the RT the pick pass writes
    bgfx::TextureHandle pickReadback = BGFX_INVALID_HANDLE;  ///< BLIT_DST|READ_BACK copy

    /// Blit a render target into its readback texture and pull the bytes back.
    ///
    /// Bounded frame pumping, because bgfx readback is asynchronous: readTexture reports the frame
    /// number at which the data becomes valid, and the documented way to wait is to keep
    /// submitting frames until we reach it.
    kernel::Result<std::vector<std::uint8_t>> readTarget(bgfx::TextureHandle target,
                                                        bgfx::TextureHandle readback,
                                                        std::uint32_t w, std::uint32_t h);

    IFrameSink::Stats stats;

    /// Uploads one batch's instance data and submits it. Shared by the shaded and pick passes,
    /// because a pick that draws a different set of geometry than the view is a pick that lies.
    std::uint32_t submitBatches(const SceneFrame& frame, bgfx::ViewId view,
                                bgfx::ProgramHandle program, std::uint64_t state);

    /// Re-applies the uniforms every shaded draw needs.
    ///
    /// PER DRAW, not per frame. `bgfx::submit` defaults to BGFX_DISCARD_ALL, and
    /// BGFX_DISCARD_STATE is documented as discarding "state and uniform bindings" — so a
    /// uniform set once before a loop of submits reaches the FIRST draw and nothing else. Every
    /// batch after the first was drawing with undefined ambient and highlight. Invisible in the
    /// single-batch offscreen spike, and in a real assembly it reads as inconsistent lighting
    /// rather than as a bug.
    void applyShadingUniforms();

    /// End a frame.
    ///
    /// Just `bgfx::frame()`. An earlier version also called `bgfx::renderFrame()` here, on the
    /// theory that single-threaded mode splits queue from execute. That is backwards: calling
    /// renderFrame() before init selects single-threaded mode, and thereafter `frame()` renders
    /// by itself. The extra call rendered a second, empty frame and then segfaulted.
    ///
    /// Single-threaded mode is gone too — it was a speculative fix for a hang whose actual cause
    /// (a 0x0 backbuffer with a live window) is fixed. Multi-threaded is bgfx's default and
    /// best-tested path; deviating from it cost two regressions and bought nothing.
    std::uint32_t advanceFrame() { return bgfx::frame(); }
};

kernel::Result<std::vector<std::uint8_t>> BgfxBackend::Impl::readTarget(
    bgfx::TextureHandle target, bgfx::TextureHandle readback, std::uint32_t w, std::uint32_t h) {
    if (!bgfx::isValid(target) || !bgfx::isValid(readback) || w == 0 || h == 0) {
        return Error{ErrorCode::InvalidInput, "There is nothing to read back."};
    }
    if ((bgfx::getCaps()->supported & BGFX_CAPS_TEXTURE_BLIT) == 0) {
        return Error{ErrorCode::Unsupported,
                     "This graphics device cannot copy textures.",
                     "BGFX_CAPS_TEXTURE_BLIT is not supported by the active renderer"};
    }
    if ((bgfx::getCaps()->supported & BGFX_CAPS_TEXTURE_READ_BACK) == 0) {
        return Error{ErrorCode::Unsupported,
                     "This graphics device cannot read textures back.",
                     "BGFX_CAPS_TEXTURE_READ_BACK is not supported by the active renderer"};
    }

    bgfx::blit(kViewBlit, readback, 0, 0, target);

    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(w) * h * 4);
    const std::uint32_t readyFrame = bgfx::readTexture(readback, pixels.data());

    // advanceFrame returns the frame number just completed, so loop while we are behind. Bounded
    // so a driver that never reports ready cannot hang the caller; the blit needs at least one
    // frame, so this always runs at least once.
    for (int guard = 0; guard < 16; ++guard) {
        if (advanceFrame() >= readyFrame) break;
    }
    return pixels;
}

void BgfxBackend::Impl::applyShadingUniforms() {
    // PER DRAW, with the others, and for the same reason: bgfx::submit discards uniform bindings,
    // so a section plane set once before the batch loop would clip the first draw and nothing else
    // — which looks like the cut applying to one body in an assembly.
    bgfx::setUniform(uSectionPlane, sectionPlane);

    const float shading[4]{config.ambient, 0, 0, 0};
    if (bgfx::isValid(uShading)) bgfx::setUniform(uShading, shading);
    // u_highlight now carries the LOOKUP GEOMETRY, not a colour: xy are the texture dimensions the
    // shader needs to turn an element slot into a texel, z is whether a lookup is available at all.
    // The tint per highlight kind lives in the shader, because it has to vary per fragment and a
    // uniform cannot do that -- which was the original mistake.
    const float lut[4]{static_cast<float>(kHighlightLutWidth),
                       static_cast<float>(highlightLutHeight),
                       bgfx::isValid(highlightLut) ? 1.0f : 0.0f, 0.0f};
    if (bgfx::isValid(uHighlight)) bgfx::setUniform(uHighlight, lut);
    if (bgfx::isValid(sHighlight) && bgfx::isValid(highlightLut)) {
        bgfx::setTexture(0, sHighlight, highlightLut);
    }
}

void BgfxBackend::Impl::syncHighlights(std::span<const Highlight> highlights) {
    if (highlights.empty()) {
        highlightLutHeight = 0;
        return;
    }

    const std::uint16_t rows = static_cast<std::uint16_t>(
        (highlights.size() + kHighlightLutWidth - 1) / kHighlightLutWidth);

    // Recreated only when it must GROW, and never shrunk: a texture destroyed and recreated while
    // the previous frame is still in flight is a use-after-free bgfx cannot warn about, and the
    // rows are a few kilobytes each.
    if (!bgfx::isValid(highlightLut) || rows > highlightLutHeight) {
        if (bgfx::isValid(highlightLut)) bgfx::destroy(highlightLut);
        highlightLutHeight = rows;
        highlightLut = bgfx::createTexture2D(kHighlightLutWidth, highlightLutHeight, false, 1,
                                             bgfx::TextureFormat::R8,
                                             BGFX_SAMPLER_POINT | BGFX_SAMPLER_UVW_CLAMP);
        highlightUploaded.clear();   // a new texture has no contents to compare against
    }

    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(kHighlightLutWidth) * highlightLutHeight,
                                    0);
    for (std::size_t i = 0; i < highlights.size(); ++i) {
        bytes[i] = static_cast<std::uint8_t>(highlights[i]);
    }
    // Skipped when nothing changed. Hover fires on every mouse-move, and re-uploading an unchanged
    // table each frame would spend bandwidth to deliver no pixels.
    if (bytes == highlightUploaded) return;

    bgfx::updateTexture2D(highlightLut, 0, 0, 0, 0, kHighlightLutWidth, highlightLutHeight,
                          bgfx::copy(bytes.data(), static_cast<std::uint32_t>(bytes.size())));
    highlightUploaded = std::move(bytes);
}

std::uint32_t BgfxBackend::Impl::submitBatches(const SceneFrame& frame, bgfx::ViewId view,
                                              bgfx::ProgramHandle program,
                                              std::uint64_t state) {
    std::uint32_t calls = 0;
    for (const Batch& batch : frame.batches) {
        if (batch.indexCount == 0 || batch.instances == BufferId::None) continue;
        const auto* vb = resources.find(batch.vertices);
        const auto* ib = resources.find(batch.indices);
        const auto* inst = resources.find(batch.instances);
        if (vb == nullptr || ib == nullptr || inst == nullptr) {
            // COUNTED, not merely skipped. A batch whose buffers do not resolve is geometry the
            // scene thinks is visible and the user cannot see.
            ++stats.skippedBatches;
            continue;
        }

        // One submit per visible run. bgfx cannot draw two disjoint runs of one buffer in a
        // single call, and an entirely visible batch is one run, so this is one call per batch
        // until culling actually fragments the assembly.
        for (const DrawRange& range : batch.ranges) {
            if (range.instanceCount == 0) continue;
            stats.instancesRequested += range.instanceCount;

            // Handle TYPE has to match how the buffer was made. bgfx handles are bare indices
            // over separate pools, so binding a dynamic buffer through a static handle is not a
            // type error — it reaches the driver as an attribute with no stride and asserts. The
            // sketch profile is the only dynamic mesh, and this is where it enters.
            // A mesh in the shared arena is bound as a WINDOW into it: startVertex acts as the
            // base vertex bgfx adds to every index, so each mesh's indices stay 0-based exactly as
            // the tessellator produced them. Without the window every index would address the
            // start of the arena and the whole scene would collapse into the first mesh.
            if (vb->arena) {
                bgfx::setVertexBuffer(0, bgfx::DynamicVertexBufferHandle{vb->idx}, vb->offset,
                                      vb->count);
            } else if (vb->kind == BgfxResources::Kind::DynamicVertex) {
                bgfx::setVertexBuffer(0, bgfx::DynamicVertexBufferHandle{vb->idx});
            } else {
                bgfx::setVertexBuffer(0, bgfx::VertexBufferHandle{vb->idx});
            }
            if (ib->arena) {
                bgfx::setIndexBuffer(bgfx::DynamicIndexBufferHandle{ib->idx},
                                     ib->offset + batch.indexOffset, batch.indexCount);
            } else if (ib->kind == BgfxResources::Kind::DynamicIndex) {
                bgfx::setIndexBuffer(bgfx::DynamicIndexBufferHandle{ib->idx}, batch.indexOffset,
                                     batch.indexCount);
            } else {
                bgfx::setIndexBuffer(bgfx::IndexBufferHandle{ib->idx}, batch.indexOffset,
                                     batch.indexCount);
            }
            // From the PERSISTENT buffer, at an offset. No copy, no per-frame budget, and
            // therefore no silent truncation.
            bgfx::setInstanceDataBuffer(bgfx::DynamicVertexBufferHandle{inst->idx},
                                        range.instanceOffset, range.instanceCount);
            applyShadingUniforms();
            // No backface culling, deliberately. Imported CAD geometry has inconsistent face
            // winding — foreign STEP and IGES routinely mix orientations — and the shader
            // already lights both sides. Culling would buy nothing except a second silent way
            // to render an empty frame. Revisit only with a measurement showing overdraw
            // actually costs us.
            // A translucent batch blends and does NOT write depth. Writing depth would occlude
            // the model in the depth buffer while still showing it through the colour buffer, so
            // edges drawn afterwards would disappear for no visible reason.
            std::uint64_t drawState = state;
            if (batch.blended) {
                drawState = (drawState & ~BGFX_STATE_WRITE_Z) | BGFX_STATE_BLEND_ALPHA;
            }
            if (batch.onTop) {
                drawState = (drawState & ~BGFX_STATE_DEPTH_TEST_MASK)
                            | BGFX_STATE_DEPTH_TEST_ALWAYS;
            }
            bgfx::setState(drawState);
            bgfx::submit(view, program);

            ++calls;
            stats.instances += range.instanceCount;
            stats.triangles += (batch.indexCount / 3) * range.instanceCount;
        }
    }
    return calls;
}

class BgfxFrameSink final : public IFrameSink {
public:
    explicit BgfxFrameSink(BgfxBackend::Impl& impl) : impl_(impl) {}

    void resize(const Viewport& v) override {
        const Viewport was = impl_.config.viewport;
        impl_.config.viewport = v;
        if (!impl_.config.offscreen) {
            bgfx::reset(v.width, v.height, BGFX_RESET_NONE);
            return;
        }
        // Offscreen: the textures ARE the framebuffer, so they have to be rebuilt at the new
        // size. Updating config alone left captureFrame reading back at the new dimensions from
        // a texture still sized at init, which skewed every row and produced diagonal streaks.
        if (!impl_.initialised || (was.width == v.width && was.height == v.height)) return;
        impl_.destroyTargets();
        impl_.createTargets(static_cast<std::uint16_t>(std::max(v.width, 1u)),
                            static_cast<std::uint16_t>(std::max(v.height, 1u)));
    }

    void submit(const SceneFrame& frame) override;
    [[nodiscard]] Stats lastFrameStats() const override { return impl_.stats; }

private:
    BgfxBackend::Impl& impl_;
};

void BgfxFrameSink::submit(const SceneFrame& frame) {
    if (!impl_.initialised) return;
    impl_.stats = Stats{};

    const std::uint32_t w = frame.viewport.width != 0 ? frame.viewport.width
                                                     : impl_.config.viewport.width;
    const std::uint32_t h = frame.viewport.height != 0 ? frame.viewport.height
                                                       : impl_.config.viewport.height;

    const std::uint32_t clear =
        (static_cast<std::uint32_t>(frame.background[0] * 255.0f) << 24)
        | (static_cast<std::uint32_t>(frame.background[1] * 255.0f) << 16)
        | (static_cast<std::uint32_t>(frame.background[2] * 255.0f) << 8) | 0xFFu;

    bgfx::setViewRect(kViewShaded, 0, 0, static_cast<std::uint16_t>(w),
                      static_cast<std::uint16_t>(h));
    bgfx::setViewClear(kViewShaded, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, clear, 1.0f);

    // touch(), or an EMPTY scene does not clear. bgfx discards a view that receives no draw
    // calls, and a discarded view never runs its clear -- so the framebuffer silently keeps the
    // previous frame. On screen that means deleting every feature leaves the old model sitting
    // in the viewport; in the scale spike it made a culled-away baseline frame read back as an
    // identical copy of the full scene, which looked exactly like an instancing failure.
    bgfx::touch(kViewShaded);
    bgfx::setViewTransform(kViewShaded, frame.camera.view.m, frame.camera.projection.m);
    if (bgfx::isValid(impl_.colourFb)) bgfx::setViewFrameBuffer(kViewShaded, impl_.colourFb);

    // Before any pass that reads it, and once per frame rather than per batch: the table covers
    // every element in the scene, not one mesh.
    impl_.syncHighlights(frame.highlights);

    // Read from the frame each submit rather than stored by a setter: the section is a property of
    // what is being drawn, and a backend holding its own copy would drift the moment a frame was
    // submitted without one.
    if (frame.sections.empty()) {
        impl_.sectionPlane[0] = impl_.sectionPlane[1] = impl_.sectionPlane[2] = 0.0f;
        impl_.sectionPlane[3] = 0.0f;
    } else {
        const SectionPlane& plane = frame.sections.front();
        impl_.sectionPlane[0] = plane.normal[0];
        impl_.sectionPlane[1] = plane.normal[1];
        impl_.sectionPlane[2] = plane.normal[2];
        impl_.sectionPlane[3] = plane.offset;
    }

    if (frame.showShaded && bgfx::isValid(impl_.shaded)) {
        impl_.stats.drawCalls += impl_.submitBatches(
            frame, kViewShaded, impl_.shaded,
            BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_WRITE_Z
                | BGFX_STATE_DEPTH_TEST_LESS | BGFX_STATE_MSAA);
    }

    if (frame.showEdges && bgfx::isValid(impl_.edge)) {
        const float edgeParams[4]{impl_.config.edgeDepthBias, 0, 0, 0};

        for (const EdgeBatch& batch : frame.edgeBatches) {
            if (batch.vertexCount == 0 || batch.instances == BufferId::None) continue;
            const auto* vb = impl_.resources.find(batch.vertices);
            const auto* inst = impl_.resources.find(batch.instances);
            if (vb == nullptr || inst == nullptr) {
                ++impl_.stats.skippedBatches;
                continue;
            }

            const float colour[4]{float(batch.colour[0]) / 255.0f,
                                  float(batch.colour[1]) / 255.0f,
                                  float(batch.colour[2]) / 255.0f, 1.0f};

            for (const DrawRange& range : batch.ranges) {
                if (range.instanceCount == 0) continue;
                // Both uniforms inside the loop: see applyShadingUniforms on why once per batch
                // is not enough.
                bgfx::setUniform(impl_.uEdgeColor, colour);
                bgfx::setUniform(impl_.uEdgeParams, edgeParams);
                bgfx::setUniform(impl_.uSectionPlane, impl_.sectionPlane);
                // The handle TYPE has to match how the buffer was created. Binding a dynamic
                // buffer through a static handle is not a type error in bgfx -- the handle is a
                // bare index, and both pools use the same numbering -- so it reaches Metal as an
                // attribute pointing at a buffer with no stride, and asserts inside the driver.
                // The sketch overlay is the only dynamic edge buffer, and this is where it enters.
                if (vb->arena) {
                    // The batch's own offset is RELATIVE to the mesh, and the mesh's offset is
                    // relative to the arena — an edge range within a mesh packed among thousands.
                    bgfx::setVertexBuffer(0, bgfx::DynamicVertexBufferHandle{vb->idx},
                                          vb->offset + batch.vertexOffset, batch.vertexCount);
                } else if (vb->kind == BgfxResources::Kind::DynamicEdge) {
                    bgfx::setVertexBuffer(0, bgfx::DynamicVertexBufferHandle{vb->idx},
                                          batch.vertexOffset, batch.vertexCount);
                } else {
                    bgfx::setVertexBuffer(0, bgfx::VertexBufferHandle{vb->idx},
                                          batch.vertexOffset, batch.vertexCount);
                }
                // The SAME buffer the shaded pass used, at the same offsets. Edges used to
                // carry their own copy of identical data, which doubled instance memory.
                bgfx::setInstanceDataBuffer(bgfx::DynamicVertexBufferHandle{inst->idx},
                                            range.instanceOffset, range.instanceCount);
                // PT_LINES, not PT_LINESTRIP. The vertex buffer is a line list — see the edge
                // loop in Tessellate.cpp — because one strip over a buffer holding every edge of
                // the mesh connects unrelated edges to each other.
                //
                // Depth-test but no depth-write: edges must not occlude each other, and writing
                // depth from a biased primitive corrupts the depth buffer for anything drawn
                // after.
                //
                // ALWAYS for an on-top batch: the sketch being edited lies on the face it is drawn
                // on and is often inside the body, so depth-testing it makes the user's own strokes
                // vanish into the part.
                bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A
                               | (batch.onTop ? BGFX_STATE_DEPTH_TEST_ALWAYS
                                              : BGFX_STATE_DEPTH_TEST_LEQUAL)
                               | BGFX_STATE_PT_LINES | BGFX_STATE_MSAA);
                bgfx::submit(kViewShaded, impl_.edge);
                ++impl_.stats.drawCalls;
                impl_.stats.lines += (batch.vertexCount / 2) * range.instanceCount;
            }
        }
    }

    impl_.advanceFrame();

    // Read timings AFTER the frame: getStats reports the frame just completed. The GPU figure is
    // the one that matters for a scale claim, and it is only meaningful with a real renderer —
    // Noop reports a plausible-looking zero.
    if (const bgfx::Stats* s = bgfx::getStats(); s != nullptr) {
        const double cpuFreq = double(s->cpuTimerFreq);
        const double gpuFreq = double(s->gpuTimerFreq);
        if (cpuFreq > 0.0) {
            impl_.stats.cpuFrameMs = 1000.0 * double(s->cpuTimeEnd - s->cpuTimeBegin) / cpuFreq;
        }
        if (gpuFreq > 0.0) {
            impl_.stats.gpuFrameMs = 1000.0 * double(s->gpuTimeEnd - s->gpuTimeBegin) / gpuFreq;
        }
    }
}

// ── picking ─────────────────────────────────────────────────────────────────────────────

class BgfxPicker final : public IPicker {
public:
    explicit BgfxPicker(BgfxBackend::Impl& impl) : impl_(impl) {}

    Hit pick(const SceneFrame& frame, std::uint32_t x, std::uint32_t y) override {
        std::vector<std::uint32_t> hits;
        readIds(frame, x, y, 1, 1, hits);
        Hit hit;
        if (!hits.empty() && hits.front() != 0) {
            hit.element = hits.front() - 1;   // 0 means "nothing here"
            hit.valid = true;
        }
        return hit;
    }

    void pickRect(const SceneFrame& frame, std::uint32_t x, std::uint32_t y, std::uint32_t w,
                  std::uint32_t h, std::vector<std::uint32_t>& out) override {
        std::vector<std::uint32_t> ids;
        readIds(frame, x, y, w, h, ids);
        out.clear();
        for (const std::uint32_t id : ids) {
            if (id != 0) out.push_back(id - 1);
        }
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
    }

private:
    void readIds(const SceneFrame&, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t,
                 std::vector<std::uint32_t>& out);

    BgfxBackend::Impl& impl_;
};

void BgfxPicker::readIds(const SceneFrame& frame, std::uint32_t x, std::uint32_t y,
                         std::uint32_t w, std::uint32_t h, std::vector<std::uint32_t>& out) {
    out.clear();
    if (!impl_.initialised || !bgfx::isValid(impl_.pick) || !bgfx::isValid(impl_.pickFb)) return;

    const std::uint32_t vw = impl_.config.viewport.width;
    const std::uint32_t vh = impl_.config.viewport.height;
    if (vw == 0 || vh == 0 || x >= vw || y >= vh) return;

    bgfx::setViewRect(kViewPick, 0, 0, static_cast<std::uint16_t>(vw),
                      static_cast<std::uint16_t>(vh));
    // Clear to zero: the id encoding reserves 0 for "nothing here", so an unwritten pixel is
    // unambiguously a miss rather than element 0.
    bgfx::setViewClear(kViewPick, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x00000000, 1.0f);
    // Same reason as the shaded view: an empty scene must clear the id target, or a pick reads
    // the ids of whatever was drawn last and reports a hit on geometry that is gone.
    bgfx::touch(kViewPick);
    bgfx::setViewFrameBuffer(kViewPick, impl_.pickFb);
    bgfx::setViewTransform(kViewPick, frame.camera.view.m, frame.camera.projection.m);

    impl_.submitBatches(frame, kViewPick, impl_.pick,
                        BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_WRITE_Z
                            | BGFX_STATE_DEPTH_TEST_LESS);
    // No MSAA on the pick target, deliberately: a resolved id is an averaged id, which decodes
    // to an element that was never under the pointer.

    // The blit lives inside readTarget. It was missing here entirely: pickReadback was created
    // and read but never written to, so every pick read an untouched texture and reported a miss.
    auto read = impl_.readTarget(impl_.pickTarget, impl_.pickReadback, vw, vh);
    if (!read) return;
    const std::vector<std::uint8_t>& pixels = read.value();

    for (std::uint32_t row = 0; row < h; ++row) {
        for (std::uint32_t col = 0; col < w; ++col) {
            const std::uint32_t px = x + col;
            const std::uint32_t py = y + row;
            if (px >= vw || py >= vh) continue;
            const std::size_t at = (static_cast<std::size_t>(py) * vw + px) * 4;
            const std::uint32_t id = static_cast<std::uint32_t>(pixels[at])
                                     | (static_cast<std::uint32_t>(pixels[at + 1]) << 8)
                                     | (static_cast<std::uint32_t>(pixels[at + 2]) << 16)
                                     | (static_cast<std::uint32_t>(pixels[at + 3]) << 24);
            out.push_back(id);
        }
    }
}

// ── lifecycle ───────────────────────────────────────────────────────────────────────────

BgfxBackend::BgfxBackend() : impl_(std::make_unique<Impl>()) {}
BgfxBackend::~BgfxBackend() { shutdown(); }

kernel::Result<void> BgfxBackend::initialise(const BgfxConfig& config) {
    if (impl_->initialised) return Error{ErrorCode::InvalidInput, "The renderer is already running."};
    impl_->config = config;

    // A window is required for ON-SCREEN rendering only. Offscreen must NOT have one — see the
    // platformData assignment below, where passing a window handle deadlocks Metal.
    if (config.nativeWindow == nullptr && !config.offscreen && config.rendererName != "noop") {
        // Refused explicitly rather than left to bgfx, which would quietly fall back to Noop:
        // init succeeds, nothing draws, and the failure surfaces as a blank viewport with no
        // error anywhere. Better to say so here.
        return Error{ErrorCode::InvalidInput,
                     "The renderer needs a window handle.",
                     "nativeWindow is null and offscreen is false. Pass a native window handle, "
                     "set offscreen=true, or set rendererName=\"noop\" for validation only."};
    }

    bgfx::Init init;
    init.type = bgfx::RendererType::Count;      // let bgfx choose
    if (config.rendererName == "noop") init.type = bgfx::RendererType::Noop;
    else if (config.rendererName == "metal") init.type = bgfx::RendererType::Metal;
    else if (config.rendererName == "vulkan") init.type = bgfx::RendererType::Vulkan;
    else if (config.rendererName == "d3d12") init.type = bgfx::RendererType::Direct3D12;

    if (config.offscreen) {
        // Headless bgfx requires a 0x0 BACKBUFFER — it rejects init outright with
        // "resolution of non-existing backbuffer can't be larger than 0x0" otherwise. Our real
        // resolution lives on the offscreen framebuffer created below, not on the swap chain
        // that does not exist. Found by running the spike; no amount of reading finds this.
        init.resolution.width = 0;
        init.resolution.height = 0;
    } else {
        init.resolution.width = std::max(config.viewport.width, 1u);
        init.resolution.height = std::max(config.viewport.height, 1u);
    }
    init.resolution.reset = BGFX_RESET_NONE;

    // NEVER pass an NSWindow or NSView here. Pass a CAMetalLayer.
    //
    // bgfx::init blocks the calling thread in renderSemWait waiting for the render thread. Given a
    // window or a view, the render thread's SwapChainMtl::init must build the CAMetalLayer on the
    // main thread, so it posts a block to the main run loop and waits for it — while the main
    // thread is the one parked in renderSemWait. Both wait forever. Diagnosed from a stack sample
    // and confirmed in renderer_mtl.mm at the `else` branch of `[NSThread isMainThread]`.
    //
    // Given a CAMetalLayer, SwapChainMtl::init assigns it directly and never touches another
    // thread, so it cannot deadlock. And the layer needs no window, which is what makes headless
    // rendering possible: Metal still refuses to initialise without one (line 557,
    // `if (NULL == ...->m_metalLayer) return false;`) but it never asks who owns it.
    void* surface = config.nativeWindow;
#if defined(__APPLE__)
    if (config.offscreen && surface == nullptr) {
        surface = createOffscreenMetalLayer(std::max(config.viewport.width, 1u),
                                           std::max(config.viewport.height, 1u));
        if (surface == nullptr) {
            return Error{ErrorCode::Internal,
                         "The renderer could not create a drawing surface.",
                         "createOffscreenMetalLayer returned null"};
        }
        impl_->ownedSurface = surface;   // released in shutdown()
    }
#endif
    init.platformData.nwh = surface;
    init.platformData.ndt = config.nativeDisplay;
    init.callback = &impl_->callback;

    if (!bgfx::init(init)) {
        return Error{ErrorCode::Internal,
                     "The graphics system could not be started.",
                     "bgfx::init returned false"};
    }

    // Reported once, because the two big scaling levers are conditional on them and a build that
    // lacks either needs to know at startup rather than when a 100k assembly is opened.
    const auto* caps = bgfx::getCaps();
    CAD_INFO(cad::log::Category::Render)
        << "renderer " << bgfx::getRendererName(caps->rendererType)
        << "  indirect=" << ((caps->supported & BGFX_CAPS_DRAW_INDIRECT) != 0)
        << "  compute=" << ((caps->supported & BGFX_CAPS_COMPUTE) != 0)
        << "  maxVertexBuffers=" << caps->limits.maxVertexBuffers
        << "  maxIndexBuffers=" << caps->limits.maxIndexBuffers
        << "  maxDynamicVertexBuffers=" << caps->limits.maxDynamicVertexBuffers;

    impl_->initialised = true;

    impl_->uShading = bgfx::createUniform("u_shading", bgfx::UniformType::Vec4);
    impl_->uHighlight = bgfx::createUniform("u_highlight", bgfx::UniformType::Vec4);
    impl_->uEdgeParams = bgfx::createUniform("u_edgeParams", bgfx::UniformType::Vec4);
    impl_->uEdgeColor = bgfx::createUniform("u_edgeColor", bgfx::UniformType::Vec4);
    impl_->uSectionPlane = bgfx::createUniform("u_sectionPlane", bgfx::UniformType::Vec4);
    impl_->sHighlight = bgfx::createUniform("s_highlight", bgfx::UniformType::Sampler);

    const auto program = [](const char* vs, const char* fs) -> bgfx::ProgramHandle {
        bgfx::ShaderHandle v = loadShader(vs);
        bgfx::ShaderHandle f = loadShader(fs);
        if (!bgfx::isValid(v) || !bgfx::isValid(f)) {
            // Destroy whichever half loaded, or a missing shader leaks its partner.
            if (bgfx::isValid(v)) bgfx::destroy(v);
            if (bgfx::isValid(f)) bgfx::destroy(f);
            return bgfx::ProgramHandle{bgfx::kInvalidHandle};
        }
        return bgfx::createProgram(v, f, /*destroyShaders*/ true);
    };
    impl_->shaded = program("vs_shaded", "fs_shaded");
    impl_->edge = program("vs_edge", "fs_edge");
    impl_->pick = program("vs_shaded", "fs_pick");

    // A missing shader is reported, not silently tolerated: a viewport that initialises and
    // draws nothing is the hardest possible thing to diagnose from a bug report.
    if (bgfx::getRendererType() != bgfx::RendererType::Noop && !bgfx::isValid(impl_->shaded)) {
        // Tear down before returning, or bgfx reports leaked blocks at exit. Checking the
        // ACTUAL renderer type rather than the requested one matters: asking for auto with no
        // window silently yields Noop, and comparing against the request missed that.
        const std::string expected = shaderPath("vs_shaded");
        shutdown();
        return Error{ErrorCode::Unsupported,
                     "The renderer's shaders could not be loaded.",
                     "expected compiled shaders under " + expected};
    }

    const std::uint16_t w = static_cast<std::uint16_t>(std::max(config.viewport.width, 1u));
    const std::uint16_t h = static_cast<std::uint16_t>(std::max(config.viewport.height, 1u));

    impl_->createTargets(w, h);
    return {};
}

void BgfxBackend::shutdown() {
    if (!impl_ || !impl_->initialised) return;
    // BGFX_INVALID_HANDLE is a braced initialiser, so it cannot be assigned through a
    // deduced reference — spell the reset out.
    const auto drop = [](auto& handle) {
        using H = std::decay_t<decltype(handle)>;
        if (bgfx::isValid(handle)) bgfx::destroy(handle);
        handle = H{bgfx::kInvalidHandle};
    };
    drop(impl_->shaded);
    drop(impl_->edge);
    drop(impl_->pick);
    drop(impl_->uShading);
    drop(impl_->uHighlight);
    drop(impl_->uEdgeParams);
    drop(impl_->uEdgeColor);
    drop(impl_->uSectionPlane);
    // The framebuffers own their attachment textures (createFrameBuffer with destroyTextures =
    // true), so colourTarget and pickTarget must NOT be destroyed here — only the readback copies,
    // which we created standalone.
    drop(impl_->colourFb);
    drop(impl_->colourReadback);
    drop(impl_->pickFb);
    drop(impl_->pickReadback);
    impl_->colourTarget = bgfx::TextureHandle{bgfx::kInvalidHandle};
    impl_->pickTarget = bgfx::TextureHandle{bgfx::kInvalidHandle};
    impl_->resources.releaseAll();
    bgfx::shutdown();
    impl_->initialised = false;
#if defined(__APPLE__)
    // After bgfx::shutdown, not before: bgfx releases its own reference during shutdown and
    // dropping ours first would leave it presenting into freed memory.
    if (impl_->ownedSurface != nullptr) {
        destroyMetalLayer(impl_->ownedSurface);
        impl_->ownedSurface = nullptr;
    }
#endif
}

bool BgfxBackend::ready() const noexcept { return impl_ && impl_->initialised; }

std::string BgfxBackend::rendererName() const {
    if (!ready()) return "uninitialised";
    return bgfx::getRendererName(bgfx::getRendererType());
}

bool BgfxBackend::homogeneousDepth() const {
    if (!ready()) return false;
    return bgfx::getCaps()->homogeneousDepth;
}

bool BgfxBackend::programsReady() const {
    return ready() && bgfx::isValid(impl_->shaded) && bgfx::isValid(impl_->edge)
           && bgfx::isValid(impl_->pick);
}

Backend BgfxBackend::handle() noexcept {
    static BgfxFrameSink sink(*impl_);
    static BgfxPicker picker(*impl_);
    Backend b;
    b.resources = &impl_->resources;
    b.frames = &sink;
    b.picker = &picker;
    b.name = std::string("bgfx-") + rendererName();
    return b;
}

void BgfxBackend::Impl::createTargets(std::uint16_t w, std::uint16_t h) {
    if (config.offscreen) {
        bgfx::TextureHandle colour = bgfx::createTexture2D(
            w, h, false, 1, bgfx::TextureFormat::RGBA8,
            BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
        bgfx::TextureHandle depth = bgfx::createTexture2D(
            w, h, false, 1, bgfx::TextureFormat::D24S8, BGFX_TEXTURE_RT_WRITE_ONLY);
        bgfx::TextureHandle attachments[]{colour, depth};
        colourFb = bgfx::createFrameBuffer(2, attachments, true);
        colourTarget = colour;   // owned by the framebuffer; do not destroy separately
        colourReadback = bgfx::createTexture2D(
            w, h, false, 1, bgfx::TextureFormat::RGBA8,
            BGFX_TEXTURE_BLIT_DST | BGFX_TEXTURE_READ_BACK);
    }

    if (bgfx::isValid(pick)) {
        bgfx::TextureHandle ids = bgfx::createTexture2D(
            w, h, false, 1, bgfx::TextureFormat::RGBA8,
            BGFX_TEXTURE_RT | BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT);
        bgfx::TextureHandle depth = bgfx::createTexture2D(
            w, h, false, 1, bgfx::TextureFormat::D24S8, BGFX_TEXTURE_RT_WRITE_ONLY);
        bgfx::TextureHandle attachments[]{ids, depth};
        pickFb = bgfx::createFrameBuffer(2, attachments, true);
        pickTarget = ids;

        // A SEPARATE readback texture, blitted into. readTexture requires BGFX_TEXTURE_READ_BACK,
        // which a render target does not have — reading the RT directly asserts inside bgfx
        // rather than returning an error.
        pickReadback = bgfx::createTexture2D(
            w, h, false, 1, bgfx::TextureFormat::RGBA8,
            BGFX_TEXTURE_BLIT_DST | BGFX_TEXTURE_READ_BACK);
    }
}

void BgfxBackend::Impl::destroyTargets() {
    // The framebuffers were created with destroyTextures=true, so they own their attachments and
    // colourTarget/pickTarget must NOT be destroyed here — only the readback copies, which are
    // standalone. Same ownership rule as shutdown().
    const auto drop = [](auto& handle) {
        if (bgfx::isValid(handle)) bgfx::destroy(handle);
        handle = {bgfx::kInvalidHandle};
    };
    drop(colourFb);
    drop(colourReadback);
    drop(pickFb);
    drop(pickReadback);
    colourTarget = bgfx::TextureHandle{bgfx::kInvalidHandle};
    pickTarget = bgfx::TextureHandle{bgfx::kInvalidHandle};
}

void setShaderDirectory(std::string dir) { shaderDirectory() = std::move(dir); }

kernel::Result<std::vector<std::uint8_t>> BgfxBackend::captureFrame() {
    if (!ready() || !bgfx::isValid(impl_->colourFb)) {
        return Error{ErrorCode::Unsupported,
                     "Frame capture needs the renderer in offscreen mode."};
    }
    // Not requestScreenShot: on Metal it ignores the framebuffer handle and captures the
    // backbuffer, which headless mode deliberately sizes 0x0. See DiagnosticCallback.
    return impl_->readTarget(impl_->colourTarget, impl_->colourReadback,
                             impl_->config.viewport.width, impl_->config.viewport.height);
}

}  // namespace cad::render
