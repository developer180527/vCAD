#include "cad/render/Scene.h"

#include <algorithm>
#include <limits>

namespace cad::render {
namespace {

constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

void mix(std::uint64_t& h, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        h ^= (v >> (i * 8)) & 0xFFu;
        h *= kFnvPrime;
    }
}

/// Digest of the placement list, so `update` can tell "the camera moved" from "the assembly
/// changed" without diffing megabytes.
std::uint64_t digestOf(std::span<const Placement> placements) {
    std::uint64_t h = kFnvOffset;
    for (const auto& p : placements) {
        mix(h, p.object.value);
        for (float f : p.transform) {
            // Quantised: floating-point noise in a transform must not force a rebuild of a
            // million instances.
            mix(h, static_cast<std::uint64_t>(static_cast<std::int64_t>(f * 1e6f)));
        }
        mix(h, p.visible ? 1u : 0u);
        for (std::uint8_t c : p.colour) mix(h, c);
    }
    return h;
}

void expandBounds(Bounds& into, const Bounds& b, const float t[12]) {
    if (!b.valid()) return;
    // All eight corners, because a rotated bound's axis-aligned extent is not the transform of
    // its min and max — a mistake that shows up as zoom-to-fit clipping rotated parts.
    for (int i = 0; i < 8; ++i) {
        const float p[3]{(i & 1) ? b.max[0] : b.min[0],
                         (i & 2) ? b.max[1] : b.min[1],
                         (i & 4) ? b.max[2] : b.min[2]};
        for (int r = 0; r < 3; ++r) {
            const float v = t[r] * p[0] + t[3 + r] * p[1] + t[6 + r] * p[2] + t[9 + r];
            into.min[r] = std::min(into.min[r], v);
            into.max[r] = std::max(into.max[r], v);
        }
    }
}

}  // namespace

SceneBuilder::SceneBuilder(MeshCache& meshes, IGpuResources& gpu)
    : meshes_(meshes), gpu_(gpu) {}

void SceneBuilder::setCamera(const Camera& c) noexcept {
    // Deliberately does NOT invalidate the batches. Orbiting a 1M-instance assembly must not
    // touch 56 MB of instance data — this separation is the reason update() and setCamera() are
    // different functions.
    frame_.camera = c;
}

void SceneBuilder::setViewport(const Viewport& v) noexcept { frame_.viewport = v; }

void SceneBuilder::setSectionPlanes(std::span<const SectionPlane> planes) {
    sections_.assign(planes.begin(), planes.end());
    frame_.sections = sections_;
}

void SceneBuilder::setHighlight(const naming::ElementName& name, Highlight h) {
    const auto it = highlightIndex_.find(name.digest());
    if (it == highlightIndex_.end()) return;
    if (it->second < highlights_.size()) highlights_[it->second] = h;
    frame_.highlights = highlights_;
}

void SceneBuilder::clearHighlights() {
    std::fill(highlights_.begin(), highlights_.end(), Highlight::None);
    frame_.highlights = highlights_;
}

std::optional<naming::ElementName> SceneBuilder::resolve(const IPicker::Hit& hit) const {
    if (!hit.valid || hit.element >= elementTable_.size()) return std::nullopt;
    const auto& name = elementTable_[hit.element];
    if (name.isNull()) return std::nullopt;
    return name;
}

std::optional<document::ObjectId> SceneBuilder::objectOf(std::uint32_t slot) const {
    if (slot >= elementOwner_.size()) return std::nullopt;
    return elementOwner_[slot];
}

kernel::Result<void> SceneBuilder::update(const document::Document& doc,
                                          std::span<const Placement> placements,
                                          const TessellationSettings& settings) {
    const std::uint64_t docDigest = doc.digest();
    const std::uint64_t placementDigest = digestOf(placements);

    // Early out. An idle redraw — which is most redraws — must cost nothing.
    if (built_ && docDigest == lastDocumentDigest_ && placementDigest == lastPlacementDigest_) {
        return {};
    }

    auto r = rebuild(doc, placements, settings);
    if (!r) return r;

    lastDocumentDigest_ = docDigest;
    lastPlacementDigest_ = placementDigest;
    built_ = true;
    ++stats_.rebuilds;
    return {};
}

kernel::Result<void> SceneBuilder::rebuild(const document::Document& doc,
                                           std::span<const Placement> placements,
                                           const TessellationSettings& settings) {
    batches_.clear();
    edgeBatches_.clear();
    instances_.clear();
    edgeInstances_.clear();
    elementTable_.clear();
    elementOwner_.clear();
    highlightIndex_.clear();

    for (int i = 0; i < 3; ++i) {
        bounds_.min[i] = std::numeric_limits<float>::infinity();
        bounds_.max[i] = -std::numeric_limits<float>::infinity();
    }

    // Group placements by mesh. THE dedupe step: every placement whose geometry hashes the same
    // lands in one batch and therefore one draw call, which is what makes 100k parts submit in
    // ~1000 calls (ADR 0007 scale amendment).
    struct Group {
        MeshResources* resources = nullptr;
        std::vector<std::size_t> placementIndices;
    };
    std::unordered_map<std::uint64_t, Group> groups;

    for (std::size_t pi = 0; pi < placements.size(); ++pi) {
        const Placement& p = placements[pi];
        if (!p.visible) continue;

        const auto object = doc.find(p.object);
        if (!object || object->output() == nullptr) continue;   // not computed yet; skip quietly

        auto mesh = meshes_.get(*object->output(), settings);
        if (!mesh) {
            // One unrenderable part must not blank the whole assembly — same principle as a
            // failed feature not aborting a recompute.
            ++stats_.tessellationFailures;
            continue;
        }

        // fold64: see the note on ShapeHash. lanes[0] alone is a name-only hash.
        const std::uint64_t key = mesh.value()->contentHash.fold64();
        auto& group = groups[key];
        if (group.resources == nullptr) {
            auto& res = uploaded_[key];
            if (res.vertices == BufferId::None) {
                const RenderMesh& m = *mesh.value();
                res.mesh = mesh.value();
                res.vertices = gpu_.uploadVertices(m.contentHash, m.vertices);
                res.indices = gpu_.uploadIndices(m.contentHash, m.indices);
                res.edgeVertices = gpu_.uploadEdgeVertices(m.contentHash, m.edgeVertices);
                ++stats_.uploads;
            }
            group.resources = &res;
        }
        group.placementIndices.push_back(pi);
    }

    // Reserve before filling: the frame's spans point into these vectors, so a reallocation
    // partway through would leave earlier batches pointing at freed memory. This is the one
    // place in the scene layer where a missing reserve is a use-after-free rather than a
    // performance note.
    std::size_t totalInstances = 0;
    std::size_t totalElements = 0;
    for (const auto& [key, group] : groups) {
        totalInstances += group.placementIndices.size();
        if (group.resources->mesh) {
            totalElements += group.placementIndices.size() * group.resources->mesh->elements.size();
        }
    }
    instances_.reserve(totalInstances);
    edgeInstances_.reserve(totalInstances);
    elementTable_.reserve(totalElements);
    elementOwner_.reserve(totalElements);
    batches_.reserve(groups.size());
    edgeBatches_.reserve(groups.size());

    // Deterministic batch order. Iterating an unordered_map directly would reorder draw calls
    // between runs, which defeats golden-image comparison and makes GPU captures unreadable.
    std::vector<std::uint64_t> keys;
    keys.reserve(groups.size());
    for (const auto& [key, _] : groups) keys.push_back(key);
    std::sort(keys.begin(), keys.end());

    for (const std::uint64_t key : keys) {
        const Group& group = groups[key];
        const MeshResources& res = *group.resources;
        if (!res.mesh) continue;
        const RenderMesh& mesh = *res.mesh;

        const std::size_t instanceBegin = instances_.size();
        const std::size_t edgeBegin = edgeInstances_.size();

        for (const std::size_t pi : group.placementIndices) {
            const Placement& p = placements[pi];

            Instance inst;
            // TRANSPOSE INTO THE SHADER'S LAYOUT. These two are not the same 12 floats.
            //
            // Placement::transform is a column-major 4x3 affine — four columns of three floats —
            // which is what the C ABI documents and what expandBounds above reads. The instance
            // stream is three vec4s, because bgfx hands i_data0..2 to the shader as vec4s and
            // instanceTransform() rebuilds the matrix as
            //     column N = i_dataN.xyz,  translation = (i_data0.w, i_data1.w, i_data2.w)
            // so each slot is one basis column plus one translation component.
            //
            // A straight std::copy of the 12 floats regrouped them across column boundaries: an
            // identity placement arrived as three identical (1,0,0,0) slots, giving a rank-1
            // matrix that flattened every mesh onto the X axis. Draw calls were submitted,
            // triangles had zero area, and the frame came back empty with no error anywhere.
            for (int col = 0; col < 3; ++col) {
                inst.transform[col * 4 + 0] = p.transform[col * 3 + 0];
                inst.transform[col * 4 + 1] = p.transform[col * 3 + 1];
                inst.transform[col * 4 + 2] = p.transform[col * 3 + 2];
                inst.transform[col * 4 + 3] = p.transform[9 + col];   // translation component
            }
            // Placement colour is RGBA8 for compactness in the document; instance data must be
            // floats because bgfx hands the slot to the shader as a vec4. See Instance.
            for (int c = 0; c < 3; ++c) inst.colour[c] = float(p.colour[c]) / 255.0f;
            inst.elementBase = static_cast<float>(elementTable_.size());

            // Each placement gets its OWN slice of the element table, even though it shares a
            // mesh. That is what lets 50,000 identical bolts each resolve to their own faces.
            for (const auto& name : mesh.elements) {
                highlightIndex_.emplace(name.digest(),
                                        static_cast<std::uint32_t>(elementTable_.size()));
                elementTable_.push_back(name);
                elementOwner_.push_back(p.object);
            }

            instances_.push_back(inst);
            edgeInstances_.push_back(inst);
            expandBounds(bounds_, mesh.bounds, p.transform);
        }

        if (res.vertices != BufferId::None && res.indices != BufferId::None) {
            Batch b;
            b.vertices = res.vertices;
            b.indices = res.indices;
            b.indexOffset = 0;
            b.indexCount = static_cast<std::uint32_t>(mesh.indices.size());
            b.instances = std::span<const Instance>(instances_.data() + instanceBegin,
                                                    instances_.size() - instanceBegin);
            batches_.push_back(b);
        }
        if (res.edgeVertices != BufferId::None && !mesh.edges.empty()) {
            EdgeBatch e;
            e.vertices = res.edgeVertices;
            e.vertexOffset = 0;
            e.vertexCount = static_cast<std::uint32_t>(mesh.edgeVertices.size() / 3);
            e.instances = std::span<const Instance>(edgeInstances_.data() + edgeBegin,
                                                     edgeInstances_.size() - edgeBegin);
            edgeBatches_.push_back(e);
        }
    }

    highlights_.assign(elementTable_.size(), Highlight::None);

    frame_.batches = batches_;
    frame_.edgeBatches = edgeBatches_;
    frame_.highlights = highlights_;
    frame_.sections = sections_;
    frame_.elementCount = static_cast<std::uint32_t>(elementTable_.size());

    stats_.uniqueMeshes = groups.size();
    stats_.instances = instances_.size();
    stats_.elementSlots = elementTable_.size();
    return {};
}

}  // namespace cad::render
