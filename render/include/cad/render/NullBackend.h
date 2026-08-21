#pragma once

// A backend that records instead of drawing.
//
// This is not a stub for convenience — it is why the seam is three narrow interfaces
// (ADR 0007). CI has no GPU, and the scene layer is where the logic bugs live: wrong instance
// counts, uploads that should have been deduped, an element slot that maps to the wrong name.
// All of that is testable here, deterministically, on a machine with no graphics stack.
//
// It also keeps the abstraction honest. An interface with one implementation is a guess about
// what a second implementation would need; with two, the guessing stops.

#include "cad/render/Backend.h"

#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace cad::render {

class NullGpuResources final : public IGpuResources {
public:
    BufferId uploadVertices(const kernel::ShapeHash&, std::span<const CadVertex>) override;
    BufferId uploadIndices(const kernel::ShapeHash&, std::span<const std::uint32_t>) override;
    BufferId uploadEdgeVertices(const kernel::ShapeHash&, std::span<const EdgeVertex>) override;
    BufferId uploadDynamicEdgeVertices(std::uint64_t key, std::uint64_t revision,
                                       std::span<const EdgeVertex>) override;
    BufferId uploadDynamicVertices(std::uint64_t key, std::uint64_t revision,
                                   std::span<const CadVertex>) override;
    BufferId uploadDynamicIndices(std::uint64_t key, std::uint64_t revision,
                                  std::span<const std::uint32_t>) override;
    BufferId uploadInstances(std::uint64_t key, std::uint64_t revision,
                             std::span<const Instance>) override;
    void release(BufferId) override;
    [[nodiscard]] std::uint64_t residentBytes() const override { return residentBytes_; }

    /// Uploads that actually transferred bytes. A second upload of the same content hash is a
    /// no-op and does NOT count — which is exactly the property tests assert.
    [[nodiscard]] std::size_t uploadCount() const noexcept { return uploads_; }
    [[nodiscard]] std::size_t dedupedCount() const noexcept { return deduped_; }
    [[nodiscard]] std::size_t liveBuffers() const noexcept { return sizes_.size(); }

    /// Instance uploads that actually transferred bytes, counted separately from mesh uploads.
    /// The number a scale test watches: it must stay at zero across an orbit, and rise by one
    /// per EDITED batch rather than per batch in the document.
    [[nodiscard]] std::size_t instanceUploadCount() const noexcept { return instanceUploads_; }
    [[nodiscard]] std::size_t instanceSkipCount() const noexcept { return instanceSkips_; }
    /// Uploads skipped because the sketch had not changed. The only way a test can tell a working
    /// revision check from one that re-sends the whole sketch on every frame.
    [[nodiscard]] std::size_t dynamicEdgeSkipCount() const noexcept { return dynamicEdgeSkips_; }

    /// Bytes last uploaded for a dynamic edge buffer, or 0 for a key never uploaded.
    ///
    /// Exposed so a test can check the bytes against the vertex COUNT the frame draws. They must
    /// agree exactly, and for a long time they did not: the sketch overlays passed bare xyz into a
    /// buffer whose layout is 16 bytes per vertex, so the draw read past the data and rendered a
    /// fan of lines out of whatever the previous upload had left there.
    [[nodiscard]] std::uint64_t dynamicEdgeBytes(std::uint64_t key) const noexcept {
        const auto it = dynamicEdgeBuffers_.find(key);
        return it == dynamicEdgeBuffers_.end() ? 0u : it->second.bytes;
    }
    void resetStats() noexcept {
        uploads_ = deduped_ = instanceUploads_ = instanceSkips_ = dynamicEdgeSkips_ = 0;
    }

private:
    BufferId intern(const kernel::ShapeHash&, int kind, std::uint64_t bytes);

    /// A persistent instance buffer: the handle outlives the frame, and the revision is what
    /// lets a repeat upload be skipped.
    struct InstanceBuffer {
        BufferId id = BufferId::None;
        std::uint64_t revision = 0;
        std::uint64_t bytes = 0;
    };

    std::unordered_map<std::string, BufferId> byContent_;
    std::unordered_map<std::uint64_t, InstanceBuffer> instanceBuffers_;   ///< keyed by batch key
    std::unordered_map<std::uint64_t, InstanceBuffer> dynamicEdgeBuffers_;  ///< keyed by owner
    std::unordered_map<std::uint64_t, InstanceBuffer> dynamicMeshBuffers_;  ///< vertices and indices
    std::unordered_map<std::uint64_t, std::uint64_t> sizes_;
    std::uint64_t next_ = 1;
    std::uint64_t residentBytes_ = 0;
    std::size_t uploads_ = 0;
    std::size_t deduped_ = 0;
    std::size_t instanceUploads_ = 0;
    std::size_t instanceSkips_ = 0;
    std::size_t dynamicEdgeSkips_ = 0;
};

class NullFrameSink final : public IFrameSink {
public:
    void resize(const Viewport& v) override { viewport_ = v; }
    void submit(const SceneFrame&) override;
    [[nodiscard]] Stats lastFrameStats() const override { return stats_; }

    /// A copy of the last frame's structure — counts and ids, not geometry. Enough to assert
    /// what would have been drawn without pretending to be a rasteriser.
    struct Recorded {
        std::vector<BufferId> vertexBuffers;
        std::vector<std::uint32_t> instanceCounts;
        std::uint32_t edgeBatches = 0;
        std::uint32_t highlighted = 0;
        std::uint32_t elementCount = 0;
        bool orthographic = false;
    };
    [[nodiscard]] const Recorded& recorded() const noexcept { return recorded_; }
    [[nodiscard]] std::size_t frameCount() const noexcept { return frames_; }

private:
    Viewport viewport_;
    Stats stats_;
    Recorded recorded_;
    std::size_t frames_ = 0;
};

/// Picking with no GPU.
///
/// Returns a hit the test scripts with `setNextHit`, rather than attempting a CPU rasteriser.
/// That is a deliberate boundary: what the scene layer owns is turning (instance, slot) into an
/// `ElementName`, and that is what these tests verify. Whether the GPU writes the right ids is
/// M3.3's problem and needs a GPU to answer honestly.
class NullPicker final : public IPicker {
public:
    Hit pick(const SceneFrame&, std::uint32_t x, std::uint32_t y) override;
    void pickRect(const SceneFrame&, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t,
                  std::vector<std::uint32_t>& out) override;
    void pickAperture(const SceneFrame&, std::uint32_t, std::uint32_t, std::uint32_t,
                      std::vector<ApertureHit>& out) override;

    void setNextHit(Hit h) noexcept { next_ = h; }

    /// Scripts what an aperture read returns, offsets and all.
    ///
    /// The null picker has no rasteriser by design, so it cannot answer "what is near this point"
    /// — but the RANKING above it is the interesting logic and must be testable without a GPU.
    /// This is how a test says "these three elements were under the finger, at these distances".
    void setNextAperture(std::vector<ApertureHit> hits) { aperture_ = std::move(hits); }
    [[nodiscard]] std::size_t pickCount() const noexcept { return picks_; }

private:
    Hit next_{};
    std::vector<ApertureHit> aperture_;
    std::size_t picks_ = 0;
};

/// The three bundled together, for a test or a headless tool.
class NullBackend {
public:
    NullBackend();
    [[nodiscard]] Backend handle() noexcept { return backend_; }
    NullGpuResources resources;
    NullFrameSink frames;
    NullPicker picker;

private:
    Backend backend_;
};

}  // namespace cad::render
