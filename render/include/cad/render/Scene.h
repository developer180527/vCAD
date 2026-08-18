#pragma once

#include "cad/document/Document.h"
#include "cad/render/Backend.h"
#include "cad/render/Tessellate.h"

#include <optional>
#include <unordered_map>
#include <vector>

namespace cad::render {

/// One placement of a document object. The shell supplies these; the builder turns them into
/// batches. Separated from `document::ObjectData` because an assembly places the same part
/// many times, and the document node is the part, not the placement.
struct Placement {
    document::ObjectId object;
    float transform[12]{1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};   ///< column-major 4x3
    std::uint8_t colour[4]{191, 194, 199, 255};
    bool visible = true;
};

/// Builds `SceneFrame`s from a document, and owns the CPU-side buffers the frame spans over.
///
/// The performance contract, which is the whole point of this class:
///
///   * **Camera-only changes rebuild nothing.** Orbiting a 1M-instance assembly must not touch
///     56 MB of instance data. `setCamera` is separate from `update` for exactly this reason.
///   * **An unchanged document rebuilds nothing.** `update` compares the document digest and
///     returns early. Idle redraws are free.
///   * **Uploads are deduplicated by mesh content hash.** N identical parts upload one buffer,
///     the same property that makes tessellation cheap (ADR 0007 scale amendment).
class SceneBuilder {
public:
    SceneBuilder(MeshCache& meshes, IGpuResources& gpu);

    /// Rebuilds batches if the document or the placement list changed. Cheap and idempotent
    /// when nothing has.
    kernel::Result<void> update(const document::Document&, std::span<const Placement>,
                               const TessellationSettings& = {});

    /// Sets the camera and re-culls. Deliberately does NOT rebuild instance data — culling only
    /// recomputes which contiguous runs of the persistent instance buffers are drawn, which is
    /// thousands of cell tests rather than millions of instance writes.
    void setCamera(const Camera&) noexcept;
    void setViewport(const Viewport&) noexcept;
    void setSectionPlanes(std::span<const SectionPlane>);

    /// Culling controls.
    ///
    /// `minPixels` is small-feature culling: a part covering less than this on screen is skipped.
    /// A 100k-part assembly zoomed out has most of its parts covering under a pixel, and drawing
    /// them costs the full triangle count for sub-pixel results. Zero disables it.
    struct CullSettings {
        bool frustum = true;
        float minPixels = 2.0f;
    };
    void setCullSettings(const CullSettings&) noexcept;

    /// Viewport clear colour, linear 0..1. Here rather than in BgfxConfig because it is a per
    /// frame property the shell changes with the theme, not a device setting fixed at init.
    void setBackground(float r, float g, float b) noexcept;

    struct CullStats {
        std::size_t cells = 0;              ///< spatial cells across all batches
        std::size_t cellsVisible = 0;
        std::size_t instancesVisible = 0;
        std::size_t instancesTotal = 0;
        std::size_t ranges = 0;             ///< draw ranges emitted; this becomes the draw count
        double lastCullMs = 0.0;
    };
    [[nodiscard]] const CullStats& cullStats() const noexcept { return cullStats_; }

    /// Marks an element. Cheap — writes one byte in the highlight table rather than rebuilding
    /// anything, because hover fires on every mouse move.
    ///
    /// By NAME: marks that element in every placement of the mesh, because an `ElementName`
    /// identifies geometry in the document and the document does not know about placements.
    /// Costs one hash lookup per placement.
    /// Draws loose world-space line segments over the scene: x,y,z per endpoint, two per segment.
    ///
    /// For the sketch being edited. That sketch is NOT in the document until it is finished, so
    /// nothing else in the frame draws it and without this the user draws blind.
    ///
    /// Unlike every other batch here it has no mesh, no placements and no culling — it is a handful
    /// of lines in world space. It rides through the ordinary edge shader on a single identity
    /// instance, which costs one 48-byte upload and avoids a second shader variant that would then
    /// need its own pipeline state on every backend.
    ///
    /// `revision` must change when the lines change and NOT when the camera moves, or an orbit
    /// re-uploads the sketch every frame. Pass an empty span to clear.
    void setSketchOverlay(std::span<const float> lineVertices, std::uint64_t revision);

    /// The half-drawn shape, drawn dimmer and thinner than the committed sketch.
    ///
    /// Its own batch because the edge shader takes its colour from a per-batch uniform, so one
    /// batch cannot hold two colours. The dashes are geometry — the caller sends short segments —
    /// which keeps the shader free of a stipple pattern that every backend would have to agree on.
    void setSketchPreview(std::span<const float> lineVertices, std::uint64_t revision);

    void setHighlight(const naming::ElementName&, Highlight);

    /// By absolute element SLOT, as returned by a pick. O(1), and the only one hover should use
    /// — it marks the one placement the user is actually pointing at.
    void setHighlight(std::uint32_t elementSlot, Highlight);

    void clearHighlights();

    /// A pick result becomes a stable geometric reference here. This is the payoff of carrying
    /// an element index in every vertex: the GPU returns a slot, and this turns it into an
    /// `ElementName` that survives a rebuild.
    [[nodiscard]] std::optional<naming::ElementName> resolve(const IPicker::Hit&) const;

    /// Which document object an element slot belongs to — what the shell needs to select a part
    /// in the tree from a click in the viewport.
    [[nodiscard]] std::optional<document::ObjectId> objectOf(std::uint32_t elementSlot) const;

    [[nodiscard]] const SceneFrame& frame() const noexcept { return frame_; }
    [[nodiscard]] Bounds bounds() const noexcept { return bounds_; }

    struct Stats {
        std::size_t rebuilds = 0;          ///< how many times update() did real work
        std::size_t uploads = 0;           ///< buffer uploads issued
        std::size_t uniqueMeshes = 0;
        std::size_t instances = 0;
        std::size_t elementSlots = 0;
        std::size_t tessellationFailures = 0;
    };
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }
    void resetStats() noexcept { stats_ = Stats{}; }

private:
    struct MeshResources {
        BufferId vertices = BufferId::None;
        BufferId indices = BufferId::None;
        BufferId edgeVertices = BufferId::None;
        RenderMeshPtr mesh;
        /// Element name digest -> index within THIS mesh's element list. One entry per element
        /// per unique mesh — tens of entries, not tens of millions. Used by the name-based
        /// highlight path so it does not need a frame-global name index.
        std::unordered_map<std::uint64_t, std::uint32_t> localOfDigest;
    };

    /// One placement's element range, and how to interpret it.
    ///
    /// This REPLACES a frame-global table of concatenated ElementNames. That table held
    /// instances x elements-per-mesh entries — 1.44M ElementName objects and a matching hash map
    /// for 100k parts, measured at 265 MB and 3.9 s to build, and heading for multiple GB at 1M.
    ///
    /// Every slice is contiguous and exactly `mesh->elements.size()` long, so a picked id can be
    /// resolved by binary search over the bases plus one subtraction. The names themselves already
    /// live once per unique mesh; materialising a copy per placement was pure waste.
    struct InstanceSlice {
        std::uint32_t elementBase = 0;
        const MeshResources* resources = nullptr;
        document::ObjectId object;
    };

    /// Rebuilds the extra edge batches that draw highlighted edges in their highlight colour.
    ///
    /// Separate from `rebuild` because highlights change WITHOUT the scene changing — a hover is
    /// not a document edit — and separate from the base edge batches because an edge's colour is
    /// per draw call while the base batch covers every edge of a mesh at once. That is the same
    /// constraint that made `u_highlight` unable to express "this face and not the other five";
    /// for edges the answer is a second draw over a vertex sub-range, which `EdgeRange` already
    /// describes, so it needs no change to the mesh format.
    ///
    /// Must run AFTER `cull()`, because it copies each base batch's draw ranges.
    void refreshEdgeHighlights();

    /// A spatial bucket of instances within one batch, contiguous in the instance buffer.
    struct Cell {
        Bounds bounds;
        std::uint32_t instanceOffset = 0;
        std::uint32_t instanceCount = 0;
        float radius = 0.0f;      ///< bounding-sphere radius, for the screen-size test
        float centre[3]{};
    };

    kernel::Result<void> rebuild(const document::Document&, std::span<const Placement>,
                                 const TessellationSettings&);
    /// Recomputes visible draw ranges from the current camera. Called by setCamera/setViewport.
    void cull() noexcept;
    /// The slice containing an absolute element slot, or null. Binary search over `slices_`.
    [[nodiscard]] const InstanceSlice* sliceFor(std::uint32_t element) const noexcept;

    MeshCache& meshes_;
    IGpuResources& gpu_;

    /// Keyed by mesh content hash. This map IS the dedupe: 50,000 identical bolts find the same
    /// entry and upload nothing.
    std::unordered_map<std::uint64_t, MeshResources> uploaded_;

    // Storage the frame's spans point into. Held here so a frame is a view, never a copy.
    std::vector<Batch> batches_;
    std::vector<EdgeBatch> edgeBatches_;
    /// One copy of the instance data, shared by the shaded and edge batches.
    std::vector<Instance> instances_;
    std::vector<SectionPlane> sections_;
    std::vector<Highlight> highlights_;

    std::vector<InstanceSlice> slices_;   ///< sorted by elementBase, one per drawn placement
    std::uint32_t elementCount_ = 0;

    /// Cells per GROUP — one group is one unique mesh, and its shaded and edge batches share
    /// both the instance buffer and the cell list. `ranges_` is one flat vector that every
    /// batch's span points into, reserved during rebuild so culling never reallocates it (a
    /// reallocation would dangle every span already handed to the frame).
    std::vector<std::vector<Cell>> cells_;
    std::vector<DrawRange> ranges_;

    /// The sketch overlay's own buffers and its single draw range, kept alive between frames
    /// because the batch's span points into this.
    BufferId sketchVertices_ = BufferId::None;
    BufferId sketchInstance_ = BufferId::None;
    std::uint32_t sketchVertexCount_ = 0;
    std::vector<DrawRange> sketchRange_;
    /// Uploads the shared identity instance the two sketch batches ride on, once.
    void ensureSketchInstance();

    BufferId previewVertices_ = BufferId::None;
    std::uint32_t previewVertexCount_ = 0;
    /// Batch index -> group index, because a group contributes a shaded batch, an edge batch,
    /// both or neither, so the three vectors are not parallel.
    std::vector<std::uint32_t> batchGroup_;
    std::vector<std::uint32_t> edgeBatchGroup_;
    /// Edge batches up to this index are the base ones; everything after is a highlight
    /// overlay, rebuilt whenever the highlight set or the culling changes.
    std::size_t baseEdgeBatches_ = 0;

    CullSettings cull_;
    CullStats cullStats_;

    SceneFrame frame_;
    Bounds bounds_;
    Stats stats_;


    std::uint64_t lastDocumentDigest_ = 0;
    std::uint64_t lastPlacementDigest_ = 0;
    bool built_ = false;
};

}  // namespace cad::render
