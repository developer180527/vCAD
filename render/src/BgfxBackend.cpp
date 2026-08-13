#include "cad/render/BgfxBackend.h"

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
#include <vector>

namespace cad::render {
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
        return intern(hash, 0, v.size() * sizeof(CadVertex), [&] {
            // makeRef would alias caller memory that outlives nothing; copy is correct here
            // because RenderMesh may be evicted while the GPU buffer lives on.
            const bgfx::Memory* mem = bgfx::copy(v.data(),
                                                static_cast<std::uint32_t>(v.size_bytes()));
            const auto h = bgfx::createVertexBuffer(mem, cadVertexLayout());
            return Entry{h.idx, Kind::Vertex, v.size_bytes()};
        });
    }

    BufferId uploadIndices(const kernel::ShapeHash& hash,
                           std::span<const std::uint32_t> i) override {
        if (i.empty()) return BufferId::None;
        return intern(hash, 1, i.size_bytes(), [&] {
            const bgfx::Memory* mem = bgfx::copy(i.data(),
                                                static_cast<std::uint32_t>(i.size_bytes()));
            // BGFX_BUFFER_INDEX32: CAD meshes routinely exceed 65k vertices, and a silent
            // 16-bit truncation renders convincing garbage rather than failing.
            const auto h = bgfx::createIndexBuffer(mem, BGFX_BUFFER_INDEX32);
            return Entry{h.idx, Kind::Index, i.size_bytes()};
        });
    }

    BufferId uploadEdgeVertices(const kernel::ShapeHash& hash,
                                std::span<const float> f) override {
        if (f.empty()) return BufferId::None;
        return intern(hash, 2, f.size_bytes(), [&] {
            const bgfx::Memory* mem = bgfx::copy(f.data(),
                                                static_cast<std::uint32_t>(f.size_bytes()));
            const auto h = bgfx::createVertexBuffer(mem, edgeVertexLayout());
            return Entry{h.idx, Kind::Edge, f.size_bytes()};
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

    void release(BufferId id) override {
        const auto it = entries_.find(static_cast<std::uint64_t>(id));
        if (it == entries_.end()) return;
        switch (it->second.kind) {
            case Kind::Vertex:
            case Kind::Edge:
                bgfx::destroy(bgfx::VertexBufferHandle{it->second.idx});
                break;
            case Kind::Index:
                bgfx::destroy(bgfx::IndexBufferHandle{it->second.idx});
                break;
            case Kind::Instance:
                bgfx::destroy(bgfx::DynamicVertexBufferHandle{it->second.idx});
                break;
        }
        resident_ -= it->second.bytes;
        for (auto k = byContent_.begin(); k != byContent_.end(); ++k) {
            if (k->second == id) { byContent_.erase(k); break; }
        }
        for (auto k = instanceBuffers_.begin(); k != instanceBuffers_.end(); ++k) {
            if (k->second.id == id) { instanceBuffers_.erase(k); break; }
        }
        entries_.erase(it);
    }

    [[nodiscard]] std::uint64_t residentBytes() const override { return resident_; }

    enum class Kind : std::uint8_t { Vertex, Index, Edge, Instance };
    struct Entry {
        std::uint16_t idx = bgfx::kInvalidHandle;
        Kind kind = Kind::Vertex;
        std::size_t bytes = 0;
    };
    [[nodiscard]] const Entry* find(BufferId id) const {
        const auto it = entries_.find(static_cast<std::uint64_t>(id));
        return it == entries_.end() ? nullptr : &it->second;
    }

    /// Destroys every buffer. Without this bgfx prints "BGFX LEAK: VertexBufferHandle ..." at
    /// shutdown — which it does report, but only after the fact and only to stderr, so it is
    /// easy to ship. Called from BgfxBackend::shutdown before bgfx::shutdown.
    void releaseAll() {
        for (const auto& [id, entry] : entries_) {
            switch (entry.kind) {
                case Kind::Vertex:
                case Kind::Edge:
                    bgfx::destroy(bgfx::VertexBufferHandle{entry.idx});
                    break;
                case Kind::Index:
                    bgfx::destroy(bgfx::IndexBufferHandle{entry.idx});
                    break;
                case Kind::Instance:
                    bgfx::destroy(bgfx::DynamicVertexBufferHandle{entry.idx});
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
    std::unordered_map<std::uint64_t, InstanceSlot> instanceBuffers_;
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
    bgfx::UniformHandle uEdgeParams = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle uEdgeColor = BGFX_INVALID_HANDLE;

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
    const float shading[4]{config.ambient, 0, 0, 0};
    const float noHighlight[4]{0, 0, 0, 0};
    if (bgfx::isValid(uShading)) bgfx::setUniform(uShading, shading);
    if (bgfx::isValid(uHighlight)) bgfx::setUniform(uHighlight, noHighlight);
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
        if (vb == nullptr || ib == nullptr || inst == nullptr) continue;

        // One submit per visible run. bgfx cannot draw two disjoint runs of one buffer in a
        // single call, and an entirely visible batch is one run, so this is one call per batch
        // until culling actually fragments the assembly.
        for (const DrawRange& range : batch.ranges) {
            if (range.instanceCount == 0) continue;
            stats.instancesRequested += range.instanceCount;

            bgfx::setVertexBuffer(0, bgfx::VertexBufferHandle{vb->idx});
            bgfx::setIndexBuffer(bgfx::IndexBufferHandle{ib->idx}, batch.indexOffset,
                                 batch.indexCount);
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
            bgfx::setState(state);
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
    bgfx::setViewTransform(kViewShaded, frame.camera.view.m, frame.camera.projection.m);
    if (bgfx::isValid(impl_.colourFb)) bgfx::setViewFrameBuffer(kViewShaded, impl_.colourFb);

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
            if (vb == nullptr || inst == nullptr) continue;

            const float colour[4]{float(batch.colour[0]) / 255.0f,
                                  float(batch.colour[1]) / 255.0f,
                                  float(batch.colour[2]) / 255.0f, 1.0f};

            for (const DrawRange& range : batch.ranges) {
                if (range.instanceCount == 0) continue;
                // Both uniforms inside the loop: see applyShadingUniforms on why once per batch
                // is not enough.
                bgfx::setUniform(impl_.uEdgeColor, colour);
                bgfx::setUniform(impl_.uEdgeParams, edgeParams);
                bgfx::setVertexBuffer(0, bgfx::VertexBufferHandle{vb->idx}, batch.vertexOffset,
                                      batch.vertexCount);
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
                bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A
                               | BGFX_STATE_DEPTH_TEST_LEQUAL | BGFX_STATE_PT_LINES
                               | BGFX_STATE_MSAA);
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
    impl_->initialised = true;

    impl_->uShading = bgfx::createUniform("u_shading", bgfx::UniformType::Vec4);
    impl_->uHighlight = bgfx::createUniform("u_highlight", bgfx::UniformType::Vec4);
    impl_->uEdgeParams = bgfx::createUniform("u_edgeParams", bgfx::UniformType::Vec4);
    impl_->uEdgeColor = bgfx::createUniform("u_edgeColor", bgfx::UniformType::Vec4);

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
