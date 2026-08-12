#pragma once

// THE SWAP SEAM (ADR 0007).
//
// Three narrow interfaces, split by who owns the data — deliberately not one
// `IRenderer::render(scene)`. The engine's own architecture doc diagnoses that shape as "a
// facade: swap it and you inherit nothing", and it is right. A single coarse interface means
// whoever implements it reimplements culling, sorting, resource lifetime and picking from
// scratch, which is exactly the cost a seam is supposed to avoid.
//
// Everything ABOVE this file — tessellation, element naming, scene assembly, edge extraction
// — is ours permanently. Everything below is replaceable: the bgfx MVP now, the engine
// renderer later, a null backend in CI.
//
// Hard rules, so a replacement is genuinely droppable:
//   * POD only. No OCCT, no document types, no std::shared_ptr crossing an interface.
//   * Resources are opaque handles, never pointers. Same reasoning as the plugin ABI.
//   * No allocation per frame across the seam. A SceneFrame is spans over caller memory.
//   * Nothing throws.

#include "cad/render/RenderMesh.h"

#include <cstdint>
#include <span>
#include <string>

namespace cad::render {

/// Opaque GPU resource handles. Zero is null.
enum class BufferId : std::uint64_t { None = 0 };
enum class TextureId : std::uint64_t { None = 0 };

struct Viewport {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    float devicePixelRatio = 1.0f;
};

/// Column-major 4x4, matching bgfx and the engine's Mat4.
struct Mat4 {
    float m[16]{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
};

struct Camera {
    Mat4 view;
    Mat4 projection;
    float eye[3]{0, 0, 0};
    /// Orthographic is not a nicety in CAD: engineers check alignment in it, and perspective
    /// makes coincident faces ambiguous.
    bool orthographic = false;
};

/// How an element should be drawn differently from its neighbours. Selection and hover are
/// per-element, not per-object, because a user selects a face and not a part.
enum class Highlight : std::uint8_t { None = 0, Hovered, Selected, Error };

/// One placement of a mesh. 64 bytes.
///
/// The size is a per-frame bandwidth decision AND a hard API constraint:
///   * 4x3 affine transform, not Mat4. The fourth row of a CAD placement is always
///     (0,0,0,1); storing it would cost 16 MB per million instances to say nothing.
///   * packed RGBA8 colour, not four floats.
///   * **bgfx requires instance data stride to be a multiple of 16 bytes.** An earlier draft
///     was 56 and would simply not have uploaded. Padding to 64 is not optional, so the spare
///     space is spent on something needed rather than left as filler.
struct Instance {
    float transform[12];              ///< column-major 4x3
    std::uint8_t colour[4]{191, 194, 199, 255};

    /// Where this instance's element names start in the frame's element table.
    ///
    /// This exists because dedupe means one mesh is shared by many parts: 50,000 identical
    /// bolts are one mesh, and that mesh cannot carry 50,000 sets of element names. The mesh
    /// stores element SLOTS; the instance stores its base. A GPU pick returns
    /// (instance, slot) and resolves through this.
    std::uint32_t elementBase = 0;

    /// Index of this instance within its batch. Needed by the pick shader: the element index
    /// arrives as a vertex attribute, but the *instance* has to come from instance data, and
    /// without both an id-buffer pick cannot tell which of 50,000 identical bolts was clicked.
    std::uint32_t instanceId = 0;

    std::uint32_t reserved = 0;        ///< pad to the 16-byte stride bgfx requires
};
// Asserted because I have got this wrong twice: once with a speculative `flags` field that made
// the size 60 while the comment claimed 56, and once at 56 — which reads fine and is rejected
// outright by bgfx's instance-stride rule.
static_assert(sizeof(Instance) == 64, "bgfx instance data stride must be a multiple of 16");

/// One draw call, instanced across every part that uses this mesh.
///
/// NOT one item per part. 100k parts means 100k draw calls, which the CPU cannot submit at
/// any framerate — see the scale amendment in ADR 0007. Content-addressed dedupe is what
/// makes this tractable: ~1000 unique meshes for a 100k-part assembly.
struct Batch {
    BufferId vertices = BufferId::None;
    BufferId indices = BufferId::None;
    std::uint32_t indexOffset = 0;
    std::uint32_t indexCount = 0;
    std::span<const Instance> instances;
    bool doubleSided = false;
};

/// Edge batch. Separate stream: line primitives, own pass, own depth bias (ADR 0007
/// decision 5).
struct EdgeBatch {
    BufferId vertices = BufferId::None;
    std::uint32_t vertexOffset = 0;
    std::uint32_t vertexCount = 0;
    std::span<const Instance> instances;
    std::uint8_t colour[4]{38, 41, 46, 255};
    float widthPx = 1.5f;
};

/// A half-space that clips geometry, for section views. CAD-specific and above the seam,
/// but the backend has to know because it becomes a shader uniform.
struct SectionPlane {
    float normal[3]{0, 0, 1};
    float offset = 0.0f;
    bool capped = true;      ///< fill the cut with a solid cap rather than showing hollow
};

/// Everything needed to draw one frame. Spans over caller-owned memory, valid only for the
/// duration of the submit call.
struct SceneFrame {
    Camera camera;
    Viewport viewport;
    std::span<const Batch> batches;
    std::span<const EdgeBatch> edgeBatches;
    std::span<const SectionPlane> sections;

    /// Per-element highlight, indexed by (instance.elementBase + vertex.element). Sparse in
    /// practice — a user selects a handful of faces out of millions — so backends should treat
    /// an empty span as "nothing highlighted" rather than allocating per element.
    std::span<const Highlight> highlights;

    /// Total element slots this frame, i.e. the size of the logical element table. Backends
    /// need it to size the pick target's id range.
    std::uint32_t elementCount = 0;

    float background[4]{0.16f, 0.17f, 0.19f, 1.0f};
    bool showEdges = true;
    bool showShaded = true;
};

// ── Seam interface 1: GPU resources ─────────────────────────────────────────────────────
//
// Keyed by content hash so an unchanged mesh is never re-uploaded — which is what makes
// dragging a dimension on a 40k-face assembly cheap. The engine's gpu_resource_cache is
// already close to this shape.
class IGpuResources {
public:
    virtual ~IGpuResources() = default;

    /// Returns an existing handle if `contentHash` was already uploaded. The caller does not
    /// track this; the backend does, because only the backend knows its own memory budget.
    virtual BufferId uploadVertices(const kernel::ShapeHash& contentHash,
                                    std::span<const CadVertex>) = 0;
    virtual BufferId uploadIndices(const kernel::ShapeHash& contentHash,
                                   std::span<const std::uint32_t>) = 0;
    virtual BufferId uploadEdgeVertices(const kernel::ShapeHash& contentHash,
                                        std::span<const float>) = 0;

    virtual void release(BufferId) = 0;

    /// Bytes currently resident. Not diagnostics: the iPad build must shed buffers under
    /// memory pressure, and it cannot do that without a number.
    [[nodiscard]] virtual std::uint64_t residentBytes() const = 0;
};

// ── Seam interface 2: frame submission ──────────────────────────────────────────────────
class IFrameSink {
public:
    virtual ~IFrameSink() = default;

    virtual void resize(const Viewport&) = 0;

    /// Draw and present. The only per-frame call across the seam.
    virtual void submit(const SceneFrame&) = 0;

    struct Stats {
        std::uint32_t drawCalls = 0;
        std::uint32_t instances = 0;
        /// Ratio of instances to drawCalls is the number that says whether dedupe is working.
        /// If they are equal on a large assembly, something has defeated it.
        std::uint32_t triangles = 0;
        std::uint32_t lines = 0;
        double cpuFrameMs = 0.0;
        double gpuFrameMs = 0.0;
    };
    [[nodiscard]] virtual Stats lastFrameStats() const = 0;
};

// ── Seam interface 3: picking ───────────────────────────────────────────────────────────
//
// Separate because it needs an offscreen target and a readback — a GPU concern the scene
// layer cannot express. GPU ID-buffer picking rather than CPU ray casting against B-rep:
// simpler, faster, and box/lasso select falls out of it for free.
class IPicker {
public:
    virtual ~IPicker() = default;

    struct Hit {
        std::uint32_t instance = 0;    ///< index into the batch's instance span
        std::uint32_t element = 0;     ///< absolute slot: instance.elementBase + local
        float depth = 1.0f;
        bool valid = false;
    };

    /// Nearest element under a point, in device pixels.
    [[nodiscard]] virtual Hit pick(const SceneFrame&, std::uint32_t x, std::uint32_t y) = 0;

    /// Every element touching a rectangle. Box select.
    virtual void pickRect(const SceneFrame&, std::uint32_t x, std::uint32_t y,
                          std::uint32_t w, std::uint32_t h,
                          std::vector<std::uint32_t>& outElements) = 0;
};

/// What a backend bundles together. A replacement provides one of these; nothing above the
/// seam knows which implementation it holds.
struct Backend {
    IGpuResources* resources = nullptr;
    IFrameSink* frames = nullptr;
    IPicker* picker = nullptr;
    std::string name;             ///< "bgfx-metal", "null", "engine" — shown in diagnostics
    [[nodiscard]] bool valid() const noexcept {
        return resources != nullptr && frames != nullptr && picker != nullptr;
    }
};

}  // namespace cad::render
