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

    void setCamera(const Camera&) noexcept;
    void setViewport(const Viewport&) noexcept;
    void setSectionPlanes(std::span<const SectionPlane>);

    /// Marks an element. Cheap — writes one byte in the highlight table rather than rebuilding
    /// anything, because hover fires on every mouse move.
    void setHighlight(const naming::ElementName&, Highlight);
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
    };

    kernel::Result<void> rebuild(const document::Document&, std::span<const Placement>,
                                 const TessellationSettings&);

    MeshCache& meshes_;
    IGpuResources& gpu_;

    /// Keyed by mesh content hash. This map IS the dedupe: 50,000 identical bolts find the same
    /// entry and upload nothing.
    std::unordered_map<std::uint64_t, MeshResources> uploaded_;

    // Storage the frame's spans point into. Held here so a frame is a view, never a copy.
    std::vector<Batch> batches_;
    std::vector<EdgeBatch> edgeBatches_;
    std::vector<Instance> instances_;
    std::vector<Instance> edgeInstances_;
    std::vector<SectionPlane> sections_;
    std::vector<Highlight> highlights_;

    /// Frame-global element table: every instance's element names concatenated. `elementBase`
    /// on an instance indexes into this.
    std::vector<naming::ElementName> elementTable_;
    std::vector<document::ObjectId> elementOwner_;
    std::unordered_map<std::uint64_t, std::uint32_t> highlightIndex_;   ///< name digest -> slot

    SceneFrame frame_;
    Bounds bounds_;
    Stats stats_;

    std::uint64_t lastDocumentDigest_ = 0;
    std::uint64_t lastPlacementDigest_ = 0;
    bool built_ = false;
};

}  // namespace cad::render
