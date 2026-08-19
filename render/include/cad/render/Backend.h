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

/// One placement of a mesh. 64 bytes, and every field is a FLOAT for a reason.
///
/// bgfx instance data is delivered to the shader as `vec4`s — always. An earlier version packed
/// an RGBA8 colour and two `uint32`s into the last 16 bytes, which is tighter and completely
/// wrong: the shader reads that slot as `vec4`, so the packed colour bytes and the integer bit
/// patterns arrive reinterpreted as floats. The result is garbage colours and garbage element
/// ids — geometry in the right place, nonsense everywhere else, and nothing to indicate why.
///
/// Since bgfx also requires the stride to be a multiple of 16, packing bought nothing anyway:
/// 12 + 3 + 1 floats is exactly 64 bytes either way.
///
/// `elementBase` as a float is safe: float32 represents integers exactly up to 2^24, which is
/// 16.7M element slots — far beyond any assembly we intend to load.
struct Instance {
    /// Three vec4 slots -> i_data0..2. Slot N is (basis column N .xyz, translation[N]).
    ///
    /// NOT the same layout as Placement::transform, which is a column-major 4x3 (four columns of
    /// three). SceneBuilder transposes between them; see the comment there for what went wrong
    /// when it did not.
    float transform[12];

    /// -> i_data3.xyz. Linear 0..1.
    float colour[3]{0.75f, 0.76f, 0.78f};

    /// -> i_data3.w. Where this instance's element names start in the frame's element table.
    ///
    /// Dedupe means one mesh is shared by many parts: 50,000 identical bolts are one mesh, and
    /// that mesh cannot carry 50,000 sets of element names. The mesh stores element SLOTS; the
    /// instance stores its base. The pick shader adds the two to get an absolute slot, which is
    /// why no separate instance id is needed.
    float elementBase = 0.0f;
};
// Asserted because this has been wrong twice: once at 60 bytes with a speculative field, and
// once at 56 — which reads fine and is rejected outright by bgfx's stride rule.
static_assert(sizeof(Instance) == 64, "bgfx instance data stride must be a multiple of 16");

/// A contiguous run of instances to draw, after culling.
///
/// Instances in a batch are sorted into spatial cells at build time, so the visible subset is
/// almost always a handful of contiguous runs rather than a scatter. That is what lets culling
/// change what is drawn WITHOUT re-uploading instance data: the buffer is persistent and only
/// the offsets move.
struct DrawRange {
    std::uint32_t instanceOffset = 0;
    std::uint32_t instanceCount = 0;
};

/// One mesh, instanced across every part that uses it.
///
/// NOT one item per part. 100k parts means 100k draw calls, which the CPU cannot submit at
/// any framerate — see the scale amendment in ADR 0007. Content-addressed dedupe is what
/// makes this tractable: ~1000 unique meshes for a 100k-part assembly.
struct Batch {
    BufferId vertices = BufferId::None;
    BufferId indices = BufferId::None;
    std::uint32_t indexOffset = 0;
    std::uint32_t indexCount = 0;

    /// PERSISTENT instance buffer, not a per-frame span.
    ///
    /// It used to be a span that the backend copied into bgfx's transient instance buffer every
    /// frame. That buffer has a fixed per-frame budget (6 MB by default, so 98304 of these), and
    /// when it runs out bgfx TRUNCATES SILENTLY — a 100k-part assembly drew 98% of itself and
    /// dropped every edge, at a frame rate that looked fine. Measured by spikes/scale.
    BufferId instances = BufferId::None;
    std::uint32_t instanceCount = 0;      ///< total resident in the buffer, before culling

    /// The visible runs for this frame. Empty means nothing survived culling — draw nothing,
    /// which is different from "no culling was done".
    std::span<const DrawRange> ranges;

    bool doubleSided = false;

    /// Draw regardless of what is in front of it, exactly as EdgeBatch::onTop does.
    ///
    /// For the sketch profile. A sketch lies ON the plane it is drawn on and the body it is being
    /// drawn against usually sits between that plane and the camera — so a depth-tested fill is
    /// hidden inside the solid, and the user sees the sketch's edges (which already draw on top)
    /// enclosing nothing. The shading is meant to answer "is this closed", and an answer that only
    /// appears when nothing is in the way is not an answer.
    bool onTop = false;

    /// Alpha-blended, and not writing depth.
    ///
    /// **This does not yet make the fill see-through.** The shaded fragment shader emits alpha 1,
    /// so blending against it is opaque; per-instance alpha needs a uniform, because i_data3.w
    /// already carries the element-id base. The sketch profile is therefore an opaque fill today.
    /// It is named for what it sets, not for the look it is aiming at.
    ///
    /// The depth-write suppression is doing real work regardless: a surface drawn over the model
    /// that wrote depth would occlude it in the depth buffer while showing it through the colour
    /// buffer, so edges drawn afterwards would vanish for no visible reason.
    bool blended = false;
};

/// Edge batch. Separate stream: line primitives, own pass, own depth bias (ADR 0007
/// decision 5).
///
/// Shares the shaded batch's instance buffer: the per-instance data is identical, and holding a
/// second copy doubled instance memory for nothing (64 MB at 1M parts).
struct EdgeBatch {
    BufferId vertices = BufferId::None;
    std::uint32_t vertexOffset = 0;
    std::uint32_t vertexCount = 0;
    BufferId instances = BufferId::None;
    std::uint32_t instanceCount = 0;
    std::span<const DrawRange> ranges;
    std::uint8_t colour[4]{38, 41, 46, 255};
    float widthPx = 1.5f;

    /// Draw regardless of what is in front of it.
    ///
    /// For the sketch being edited. A sketch lies ON the face it is drawn on and often INSIDE the
    /// body — a profile on the top of a block sits at the same depth as the block's own face, and
    /// one drawn to be cut away is behind solid material. Depth-tested, it disappears into the
    /// part, and the user is drawing lines they cannot see.
    ///
    /// Every other edge batch stays depth-tested: model edges hidden behind the model SHOULD be
    /// hidden, which is what makes a shaded view readable.
    bool onTop = false;
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

    /// Uploads instance data into a buffer that PERSISTS across frames.
    ///
    /// Keyed by `key` (stable for a batch across rebuilds) and versioned by `revision`: when the
    /// revision is unchanged the backend must skip the upload entirely and return the same
    /// handle. That is what makes an orbit free — the camera moves, culling changes which ranges
    /// are drawn, and not one byte of instance data is re-sent.
    ///
    /// `revision` is a digest of the data, not a counter, so an edit that touches one part
    /// re-uploads one batch rather than every batch in the assembly.
    ///
    /// Not content-hashed like the mesh uploads, because instance data is placement data: it
    /// changes whenever the assembly is edited and is not shared between batches.
    virtual BufferId uploadInstances(std::uint64_t key, std::uint64_t revision,
                                    std::span<const Instance>) = 0;

    /// Edge vertices that CHANGE, keyed and revisioned like instance data rather than content
    /// hashed like a mesh.
    ///
    /// For geometry being edited: a sketch gains a line on every other click, and content hashing
    /// it would mint a new buffer per stroke and keep every one of them, because a content-addressed
    /// cache has no way to know the old contents will never be asked for again. Keyed by owner and
    /// versioned by a digest of the data, so an editing session occupies one buffer no matter how
    /// long it runs, and a frame where nothing changed re-uploads nothing.
    virtual BufferId uploadDynamicEdgeVertices(std::uint64_t key, std::uint64_t revision,
                                               std::span<const float>) = 0;

    /// Triangle geometry that CHANGES, keyed and revisioned like the dynamic edges.
    ///
    /// For the shaded profile of the sketch being edited: it is re-triangulated whenever a curve
    /// moves, so content hashing it would mint a mesh per stroke and keep every one.
    virtual BufferId uploadDynamicVertices(std::uint64_t key, std::uint64_t revision,
                                           std::span<const CadVertex>) = 0;
    virtual BufferId uploadDynamicIndices(std::uint64_t key, std::uint64_t revision,
                                          std::span<const std::uint32_t>) = 0;

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

        /// Instances the scene ASKED to draw, before the per-frame instance buffer ran out.
        ///
        /// Must be compared against `instances`. bgfx serves instance data from a transient
        /// buffer with a fixed per-frame budget, and when it is exhausted the submission is
        /// silently truncated — a large assembly then renders a plausible-looking fraction of
        /// itself with no error. Any report that quotes `instances` without this is lying.
        std::uint32_t instancesRequested = 0;

        /// Batches the backend could not draw because a buffer handle did not resolve.
        ///
        /// Non-zero means geometry the scene believes is on screen is NOT being drawn. It was
        /// previously an unconditional `continue` — the frame simply came out emptier, faster, and
        /// silent. Anything reading these stats should treat a non-zero value as a failure rather
        /// than a statistic.
        std::uint32_t skippedBatches = 0;

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
        /// Not filled by the GPU picker: the id buffer encodes the ABSOLUTE element slot
        /// (elementBase + local), which is all `SceneBuilder::resolve` needs. Kept for the null
        /// picker and for a future backend that can report it cheaply.
        std::uint32_t instance = 0;
        std::uint32_t element = 0;     ///< absolute slot: elementBase + local element
        float depth = 1.0f;
        bool valid = false;
    };

    /// Nearest element under a point, in device pixels.
    [[nodiscard]] virtual Hit pick(const SceneFrame&, std::uint32_t x, std::uint32_t y) = 0;

    /// Every element touching a rectangle. Box select.
    virtual void pickRect(const SceneFrame&, std::uint32_t x, std::uint32_t y,
                          std::uint32_t w, std::uint32_t h,
                          std::vector<std::uint32_t>& outElements) = 0;

    /// One element found within the aperture, WITH where it was found.
    ///
    /// The difference from `pickRect`, which is otherwise the same read: that one deduplicates into
    /// a set and throws the positions away, which is right for box select and useless for deciding
    /// which of several candidates the user meant.
    struct ApertureHit {
        std::uint32_t element = 0;   ///< absolute slot
        /// Offset from the aperture's centre, in device pixels. Signed, so a caller can rank by
        /// distance without knowing where the aperture was.
        std::int32_t dx = 0;
        std::int32_t dy = 0;
    };

    /// Every element within `radius` pixels of a point, nearest occurrence of each.
    ///
    /// The primitive touch selection needs. A finger covers ~88 device pixels on a Retina display
    /// (Apple's 44 pt minimum), so a one-pixel hit test is not a strict version of selection but a
    /// broken one — see docs/design/SELECTION.md.
    ///
    /// The pick pass renders the scene and reads the target back whatever the aperture size, so a
    /// large radius costs no more than a small one.
    ///
    /// Default implementation: none. A backend that cannot do this has no business claiming to
    /// support picking, and a silently empty default would make touch selection fail as "nothing
    /// there" on exactly the backends that need it most.
    virtual void pickAperture(const SceneFrame&, std::uint32_t x, std::uint32_t y,
                              std::uint32_t radius, std::vector<ApertureHit>& out) = 0;
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
