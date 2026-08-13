#include "cad/render/Scene.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
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

/// Column-major 4x4 multiply, matching Mat4's documented layout: element (col c, row r) is
/// m[c * 4 + r].
Mat4 multiply(const Mat4& a, const Mat4& b) {
    Mat4 out;
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) sum += a.m[k * 4 + r] * b.m[c * 4 + k];
            out.m[c * 4 + r] = sum;
        }
    }
    return out;
}

/// The four side planes of the view frustum, in world space, from a view-projection matrix
/// (Gribb–Hartmann). Each is (nx, ny, nz, d) with the normal pointing INWARD, normalised so a
/// plane test yields a real distance and can be compared against a bounding-sphere radius.
///
/// Only the sides. The near and far planes are deliberately omitted because their clip-space
/// convention differs between renderers — [0,1] on Metal, D3D and Vulkan, [-1,1] on OpenGL —
/// and Camera does not carry which one is in force. Getting that wrong culls the entire scene
/// and renders nothing, with no error (the same failure Camera.cpp documents). Skipping them
/// only means a few cells behind or beyond the camera survive to be drawn, which is a cost;
/// getting them wrong means a blank viewport, which is a bug.
struct Frustum {
    float plane[4][4]{};

    static Frustum from(const Mat4& viewProj) {
        const float* m = viewProj.m;
        // Row r of the matrix is (m[r], m[4+r], m[8+r], m[12+r]).
        const auto row = [m](int r, int i) { return m[i * 4 + r]; };
        Frustum f;
        for (int i = 0; i < 4; ++i) {
            f.plane[0][i] = row(3, i) + row(0, i);   // left:   clip.x >= -clip.w
            f.plane[1][i] = row(3, i) - row(0, i);   // right:  clip.x <=  clip.w
            f.plane[2][i] = row(3, i) + row(1, i);   // bottom
            f.plane[3][i] = row(3, i) - row(1, i);   // top
        }
        for (auto& p : f.plane) {
            const float len = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
            if (len > 1e-12f) {
                for (float& v : p) v /= len;
            }
        }
        return f;
    }

    /// True if the box is entirely outside any one plane.
    ///
    /// The "positive vertex" test: only the box corner furthest along the plane normal can be
    /// inside, so if that one is behind the plane the whole box is. Tighter than a bounding
    /// sphere and the same cost. Conservative at the corners — a box straddling two planes is
    /// kept — which is the right way to be wrong.
    [[nodiscard]] bool excludes(const Bounds& b) const noexcept {
        for (const auto& p : plane) {
            const float d = p[0] * (p[0] > 0.0f ? b.max[0] : b.min[0])
                          + p[1] * (p[1] > 0.0f ? b.max[1] : b.min[1])
                          + p[2] * (p[2] > 0.0f ? b.max[2] : b.min[2]) + p[3];
            if (d < 0.0f) return true;
        }
        return false;
    }
};

/// Digest of a batch's instance data, used as the upload revision. Content-based so an edit that
/// leaves a batch's placements alone re-uploads nothing.
std::uint64_t digestOf(std::span<const Instance> instances) {
    std::uint64_t h = kFnvOffset;
    for (const Instance& i : instances) {
        for (const float f : i.transform) mix(h, std::bit_cast<std::uint32_t>(f));
        for (const float c : i.colour) mix(h, std::bit_cast<std::uint32_t>(c));
        mix(h, std::bit_cast<std::uint32_t>(i.elementBase));
    }
    return h;
}

}  // namespace

SceneBuilder::SceneBuilder(MeshCache& meshes, IGpuResources& gpu)
    : meshes_(meshes), gpu_(gpu) {}

void SceneBuilder::setCamera(const Camera& c) noexcept {
    // Deliberately does NOT invalidate the batches. Orbiting a 1M-instance assembly must not
    // touch 56 MB of instance data — this separation is the reason update() and setCamera() are
    // different functions. Culling only rewrites draw ranges, which is per-cell work.
    frame_.camera = c;
    cull();
}

void SceneBuilder::setViewport(const Viewport& v) noexcept {
    frame_.viewport = v;
    cull();   // the screen-size test is in pixels, so a resize changes what survives it
}

void SceneBuilder::setCullSettings(const CullSettings& s) noexcept {
    cull_ = s;
    cull();
}

void SceneBuilder::setSectionPlanes(std::span<const SectionPlane> planes) {
    sections_.assign(planes.begin(), planes.end());
    frame_.sections = sections_;
}

const SceneBuilder::InstanceSlice* SceneBuilder::sliceFor(std::uint32_t element) const noexcept {
    if (slices_.empty() || element >= elementCount_) return nullptr;
    // Bases are assigned in increasing order during rebuild, so the vector is already sorted and
    // upper_bound finds the slice containing this element without any per-element storage.
    auto it = std::upper_bound(slices_.begin(), slices_.end(), element,
                               [](std::uint32_t e, const InstanceSlice& s) {
                                   return e < s.elementBase;
                               });
    if (it == slices_.begin()) return nullptr;
    --it;
    return &*it;
}

void SceneBuilder::setHighlight(const naming::ElementName& name, Highlight h) {
    // O(placements) with a hash lookup per unique mesh, not per element. Fine for programmatic
    // highlighting; hover should use the element id from the pick, which is O(1) — see the
    // overload below. Highlighting by name necessarily marks the element in EVERY placement of
    // that mesh, because an ElementName identifies geometry in the document, not on screen.
    const std::uint64_t digest = name.digest();
    for (const InstanceSlice& slice : slices_) {
        if (slice.resources == nullptr) continue;
        const auto it = slice.resources->localOfDigest.find(digest);
        if (it == slice.resources->localOfDigest.end()) continue;
        const std::uint32_t id = slice.elementBase + it->second;
        if (id < highlights_.size()) highlights_[id] = h;
    }
    frame_.highlights = highlights_;
}

void SceneBuilder::setHighlight(std::uint32_t element, Highlight h) {
    if (element >= highlights_.size()) return;
    highlights_[element] = h;
    frame_.highlights = highlights_;
}

void SceneBuilder::clearHighlights() {
    std::fill(highlights_.begin(), highlights_.end(), Highlight::None);
    frame_.highlights = highlights_;
}

std::optional<naming::ElementName> SceneBuilder::resolve(const IPicker::Hit& hit) const {
    if (!hit.valid) return std::nullopt;
    const InstanceSlice* slice = sliceFor(hit.element);
    if (slice == nullptr || slice->resources == nullptr || !slice->resources->mesh) {
        return std::nullopt;
    }
    const auto& elements = slice->resources->mesh->elements;
    const std::uint32_t local = hit.element - slice->elementBase;
    if (local >= elements.size()) return std::nullopt;
    const auto& name = elements[local];
    if (name.isNull()) return std::nullopt;
    return name;
}

std::optional<document::ObjectId> SceneBuilder::objectOf(std::uint32_t slot) const {
    const InstanceSlice* slice = sliceFor(slot);
    if (slice == nullptr) return std::nullopt;
    return slice->object;
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
    batchGroup_.clear();
    edgeBatchGroup_.clear();
    instances_.clear();
    slices_.clear();
    cells_.clear();
    ranges_.clear();
    elementCount_ = 0;

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

    // Mesh per OBJECT, resolved once for this rebuild.
    //
    // An assembly places one part many times, and `MeshCache::get` is only cheap once you are
    // past its key: computing that key content-hashes the whole B-rep. Calling it per placement
    // made a 100k-instance rebuild hash 100,000 shapes to look up 20 meshes, which measured at
    // 3.8 s — the entire scene-build cost, and nothing to do with the meshes themselves.
    // A document is immutable for the duration of a rebuild, so one lookup per object is exact.
    std::unordered_map<std::uint64_t, RenderMeshPtr> meshOfObject;

    for (std::size_t pi = 0; pi < placements.size(); ++pi) {
        const Placement& p = placements[pi];
        if (!p.visible) continue;

        RenderMeshPtr resolved;
        if (const auto memo = meshOfObject.find(p.object.value); memo != meshOfObject.end()) {
            resolved = memo->second;
            if (!resolved) continue;   // already known to be unrenderable; counted once, below
        } else {
            const auto object = doc.find(p.object);
            // Not computed yet; skip quietly. Cached as null so the next placement of the same
            // object does not repeat the lookup.
            if (!object || object->output() == nullptr) {
                meshOfObject.emplace(p.object.value, nullptr);
                continue;
            }
            auto mesh = meshes_.get(*object->output(), settings);
            if (!mesh) {
                // One unrenderable part must not blank the whole assembly — same principle as a
                // failed feature not aborting a recompute.
                ++stats_.tessellationFailures;
                meshOfObject.emplace(p.object.value, nullptr);
                continue;
            }
            resolved = mesh.value();
            meshOfObject.emplace(p.object.value, resolved);
        }

        // fold64: see the note on ShapeHash. lanes[0] alone is a name-only hash.
        const std::uint64_t key = resolved->contentHash.fold64();
        auto& group = groups[key];
        if (group.resources == nullptr) {
            auto& res = uploaded_[key];
            if (res.vertices == BufferId::None) {
                const RenderMesh& m = *resolved;
                res.mesh = resolved;
                res.vertices = gpu_.uploadVertices(m.contentHash, m.vertices);
                res.indices = gpu_.uploadIndices(m.contentHash, m.indices);
                res.edgeVertices = gpu_.uploadEdgeVertices(m.contentHash, m.edgeVertices);
                // Digest -> LOCAL element index, built once per unique mesh. The map that used
                // to live here was frame-global and held one entry per element per placement;
                // this one is bounded by the mesh's own face count no matter how many times the
                // part is placed.
                res.localOfDigest.reserve(m.elements.size());
                for (std::uint32_t e = 0; e < m.elements.size(); ++e) {
                    res.localOfDigest.emplace(m.elements[e].digest(), e);
                }
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
    for (const auto& [key, group] : groups) totalInstances += group.placementIndices.size();
    instances_.reserve(totalInstances);
    slices_.reserve(totalInstances);
    batches_.reserve(groups.size());
    edgeBatches_.reserve(groups.size());
    batchGroup_.reserve(groups.size());
    edgeBatchGroup_.reserve(groups.size());
    cells_.reserve(groups.size());

    // Deterministic batch order. Iterating an unordered_map directly would reorder draw calls
    // between runs, which defeats golden-image comparison and makes GPU captures unreadable.
    std::vector<std::uint64_t> keys;
    keys.reserve(groups.size());
    for (const auto& [key, _] : groups) keys.push_back(key);
    std::sort(keys.begin(), keys.end());

    // Scratch, reused across groups so a 1000-batch assembly does not do 1000 allocations.
    struct Item {
        std::size_t placement = 0;
        std::uint32_t cell = 0;
        Bounds world;
    };
    std::vector<Item> items;

    for (const std::uint64_t key : keys) {
        const Group& group = groups[key];
        const MeshResources& res = *group.resources;
        if (!res.mesh) continue;
        const RenderMesh& mesh = *res.mesh;

        // ── world bounds per placement, and the group's own bounds ───────────────────────
        items.clear();
        items.reserve(group.placementIndices.size());
        Bounds groupBounds;
        for (int i = 0; i < 3; ++i) {
            groupBounds.min[i] = std::numeric_limits<float>::infinity();
            groupBounds.max[i] = -std::numeric_limits<float>::infinity();
        }
        for (const std::size_t pi : group.placementIndices) {
            Item item;
            item.placement = pi;
            for (int i = 0; i < 3; ++i) {
                item.world.min[i] = std::numeric_limits<float>::infinity();
                item.world.max[i] = -std::numeric_limits<float>::infinity();
            }
            expandBounds(item.world, mesh.bounds, placements[pi].transform);
            if (item.world.valid()) {
                for (int i = 0; i < 3; ++i) {
                    groupBounds.min[i] = std::min(groupBounds.min[i], item.world.min[i]);
                    groupBounds.max[i] = std::max(groupBounds.max[i], item.world.max[i]);
                    bounds_.min[i] = std::min(bounds_.min[i], item.world.min[i]);
                    bounds_.max[i] = std::max(bounds_.max[i], item.world.max[i]);
                }
            }
            items.push_back(item);
        }

        // ── bucket into a uniform grid, so culling works on runs rather than scattered ids ──
        //
        // The grid is over the GROUP's bounds, not the scene's: a batch of 50,000 bolts spread
        // through an assembly needs its own subdivision, and one global grid would put a whole
        // batch in one cell whenever its parts happen to be co-located.
        //
        // Target ~64 instances per cell. Smaller cells cull more precisely but emit more draw
        // ranges, and a draw range is a draw call — the thing this whole design exists to keep
        // down. 64 is where the two stop trading usefully at 100k parts.
        constexpr std::size_t kInstancesPerCell = 64;
        constexpr int kMaxDivisions = 32;
        int divisions = 1;
        if (items.size() > kInstancesPerCell && groupBounds.valid()) {
            divisions = static_cast<int>(
                std::cbrt(static_cast<double>(items.size()) / double(kInstancesPerCell)));
            divisions = std::clamp(divisions, 1, kMaxDivisions);
        }
        if (divisions > 1) {
            float extent[3];
            for (int i = 0; i < 3; ++i) extent[i] = groupBounds.max[i] - groupBounds.min[i];
            for (Item& item : items) {
                if (!item.world.valid()) continue;
                int idx[3]{0, 0, 0};
                for (int i = 0; i < 3; ++i) {
                    if (extent[i] <= 0.0f) continue;
                    const float centre = (item.world.min[i] + item.world.max[i]) * 0.5f;
                    const float t = (centre - groupBounds.min[i]) / extent[i];
                    idx[i] = std::clamp(static_cast<int>(t * float(divisions)), 0, divisions - 1);
                }
                item.cell = static_cast<std::uint32_t>(idx[0] + idx[1] * divisions
                                                       + idx[2] * divisions * divisions);
            }
            // STABLE: within a cell, placements keep document order. An unstable sort would
            // reorder instances between runs and break the determinism the batch sort above
            // exists to guarantee.
            std::stable_sort(items.begin(), items.end(),
                             [](const Item& a, const Item& b) { return a.cell < b.cell; });
        }

        // ── emit instances in cell order ─────────────────────────────────────────────────
        const std::uint32_t instanceBegin = static_cast<std::uint32_t>(instances_.size());
        const std::size_t groupIndex = cells_.size();
        cells_.emplace_back();
        std::vector<Cell>& cellList = cells_.back();

        for (std::size_t i = 0; i < items.size();) {
            std::size_t j = i;
            Cell cell;
            for (int a = 0; a < 3; ++a) {
                cell.bounds.min[a] = std::numeric_limits<float>::infinity();
                cell.bounds.max[a] = -std::numeric_limits<float>::infinity();
            }
            cell.instanceOffset = static_cast<std::uint32_t>(instances_.size()) - instanceBegin;

            while (j < items.size() && items[j].cell == items[i].cell) {
                const Placement& p = placements[items[j].placement];

                Instance inst;
                // TRANSPOSE INTO THE SHADER'S LAYOUT. These two are not the same 12 floats.
                //
                // Placement::transform is a column-major 4x3 affine — four columns of three
                // floats — which is what the C ABI documents and what expandBounds above reads.
                // The instance stream is three vec4s, because bgfx hands i_data0..2 to the shader
                // as vec4s and instancePosition() applies them as
                //     column N = i_dataN.xyz,  translation = (i_data0.w, i_data1.w, i_data2.w)
                // so each slot is one basis column plus one translation component.
                //
                // A straight std::copy of the 12 floats regrouped them across column boundaries:
                // an identity placement arrived as three identical (1,0,0,0) slots, giving a
                // rank-1 matrix that flattened every mesh onto the X axis. Draw calls were
                // submitted, triangles had zero area, and the frame came back empty with no
                // error anywhere.
                for (int col = 0; col < 3; ++col) {
                    inst.transform[col * 4 + 0] = p.transform[col * 3 + 0];
                    inst.transform[col * 4 + 1] = p.transform[col * 3 + 1];
                    inst.transform[col * 4 + 2] = p.transform[col * 3 + 2];
                    inst.transform[col * 4 + 3] = p.transform[9 + col];   // translation
                }
                // Placement colour is RGBA8 for compactness in the document; instance data must
                // be floats because bgfx hands the slot to the shader as a vec4. See Instance.
                for (int c = 0; c < 3; ++c) inst.colour[c] = float(p.colour[c]) / 255.0f;
                inst.elementBase = static_cast<float>(elementCount_);

                // Each placement gets its OWN slice of the element id space, even though it
                // shares a mesh — that is what lets 50,000 identical bolts each resolve to their
                // own faces. The slice is described, not materialised: the names themselves live
                // once, on the mesh.
                slices_.push_back(InstanceSlice{elementCount_, &res, p.object});
                elementCount_ += static_cast<std::uint32_t>(mesh.elements.size());

                instances_.push_back(inst);

                if (items[j].world.valid()) {
                    for (int a = 0; a < 3; ++a) {
                        cell.bounds.min[a] = std::min(cell.bounds.min[a], items[j].world.min[a]);
                        cell.bounds.max[a] = std::max(cell.bounds.max[a], items[j].world.max[a]);
                    }
                }
                ++j;
            }

            cell.instanceCount = static_cast<std::uint32_t>(j - i);
            if (cell.bounds.valid()) {
                float diagonal = 0.0f;
                for (int a = 0; a < 3; ++a) {
                    cell.centre[a] = (cell.bounds.min[a] + cell.bounds.max[a]) * 0.5f;
                    const float half = (cell.bounds.max[a] - cell.bounds.min[a]) * 0.5f;
                    diagonal += half * half;
                }
                cell.radius = std::sqrt(diagonal);
            } else {
                // No usable bounds — an empty mesh, or a degenerate transform. An infinite
                // radius keeps it visible rather than silently culling geometry we cannot
                // measure, which is the failure mode that is impossible to notice.
                cell.radius = std::numeric_limits<float>::infinity();
            }
            cellList.push_back(cell);
            i = j;
        }

        const std::uint32_t instanceCount =
            static_cast<std::uint32_t>(instances_.size()) - instanceBegin;
        if (instanceCount == 0) continue;

        const std::span<const Instance> data(instances_.data() + instanceBegin, instanceCount);
        const BufferId instanceBuffer = gpu_.uploadInstances(key, digestOf(data), data);

        if (res.vertices != BufferId::None && res.indices != BufferId::None) {
            Batch b;
            b.vertices = res.vertices;
            b.indices = res.indices;
            b.indexOffset = 0;
            b.indexCount = static_cast<std::uint32_t>(mesh.indices.size());
            b.instances = instanceBuffer;
            b.instanceCount = instanceCount;
            // b.ranges is filled by cull(), below. A batch with no ranges draws nothing.
            batches_.push_back(b);
            batchGroup_.push_back(static_cast<std::uint32_t>(groupIndex));
        }
        if (res.edgeVertices != BufferId::None && !mesh.edges.empty()) {
            EdgeBatch e;
            e.vertices = res.edgeVertices;
            e.vertexOffset = 0;
            e.vertexCount = static_cast<std::uint32_t>(mesh.edgeVertices.size() / 3);
            // Same buffer as the shaded batch, deliberately: the per-instance data is identical
            // and a second copy doubled instance memory for nothing.
            e.instances = instanceBuffer;
            e.instanceCount = instanceCount;
            edgeBatches_.push_back(e);
            edgeBatchGroup_.push_back(static_cast<std::uint32_t>(groupIndex));
        }
    }

    // One range per cell is the worst case, so reserving that much means cull() can never
    // reallocate — and therefore never dangle a span the frame is already holding.
    std::size_t totalCells = 0;
    for (const auto& list : cells_) totalCells += list.size();
    ranges_.reserve(totalCells);

    highlights_.assign(elementCount_, Highlight::None);

    frame_.batches = batches_;
    frame_.edgeBatches = edgeBatches_;
    frame_.highlights = highlights_;
    frame_.sections = sections_;
    frame_.elementCount = elementCount_;

    stats_.uniqueMeshes = groups.size();
    stats_.instances = instances_.size();
    stats_.elementSlots = elementCount_;

    cull();
    return {};
}

void SceneBuilder::cull() noexcept {
    const auto started = std::chrono::steady_clock::now();

    ranges_.clear();
    cullStats_ = CullStats{};
    cullStats_.instancesTotal = instances_.size();

    const Frustum frustum =
        Frustum::from(multiply(frame_.camera.projection, frame_.camera.view));

    // Pixel size of a cell. Only meaningful once there is a viewport with height — before the
    // shell reports one, every projected size is zero and a naive test would cull the scene.
    const float viewportHeight = static_cast<float>(frame_.viewport.height);
    const bool sizeTest = cull_.minPixels > 0.0f && viewportHeight > 0.0f;
    const float* const P = frame_.camera.projection.m;
    const float* const V = frame_.camera.view.m;

    std::size_t bi = 0;
    std::size_t ei = 0;

    for (std::size_t g = 0; g < cells_.size(); ++g) {
        const std::uint32_t begin = static_cast<std::uint32_t>(ranges_.size());

        std::uint32_t runOffset = 0;
        std::uint32_t runCount = 0;
        const auto flush = [&] {
            if (runCount == 0) return;
            ranges_.push_back(DrawRange{runOffset, runCount});
            cullStats_.instancesVisible += runCount;
            runCount = 0;
        };

        for (const Cell& cell : cells_[g]) {
            ++cullStats_.cells;
            if (cell.instanceCount == 0) continue;

            bool visible = true;
            if (cull_.frustum && std::isfinite(cell.radius) && cell.bounds.valid()) {
                visible = !frustum.excludes(cell.bounds);
            }
            if (visible && sizeTest && std::isfinite(cell.radius)) {
                // View-space centre, then the perspective divide's w. For an orthographic
                // projection w is 1 and this reduces to a constant scale, which is correct:
                // ortho has no distance falloff.
                const float* c = cell.centre;
                const float vx = V[0] * c[0] + V[4] * c[1] + V[8] * c[2] + V[12];
                const float vy = V[1] * c[0] + V[5] * c[1] + V[9] * c[2] + V[13];
                const float vz = V[2] * c[0] + V[6] * c[1] + V[10] * c[2] + V[14];
                const float w = P[3] * vx + P[7] * vy + P[11] * vz + P[15];
                if (w > 1e-6f) {
                    // P[5] is the projection's vertical scale, so radius * P[5] / w is the
                    // cell's radius in NDC, and half the viewport height converts NDC to pixels.
                    //
                    // Per CELL, not per part: a cell is dropped only when the whole bucket is
                    // sub-pixel. Conservative on purpose — the alternative is a per-instance
                    // test, which is the million-iteration loop this design exists to avoid.
                    const float pixels = cell.radius * P[5] / w * 0.5f * viewportHeight;
                    if (pixels < cull_.minPixels) visible = false;
                }
                // w <= 0 means the cell is behind the camera or on the plane; leave it visible
                // and let the GPU clip it. Culling on a negative w is how geometry disappears
                // when you zoom into it.
            }

            if (!visible) {
                flush();
                continue;
            }
            ++cullStats_.cellsVisible;

            // Merge with the run in progress when the cells are adjacent in the buffer. Cells
            // are emitted in increasing offset order, so an entirely visible batch collapses to
            // ONE range and therefore one draw call — the case that must not regress.
            if (runCount != 0 && runOffset + runCount == cell.instanceOffset) {
                runCount += cell.instanceCount;
            } else {
                flush();
                runOffset = cell.instanceOffset;
                runCount = cell.instanceCount;
            }
        }
        flush();

        const std::uint32_t count = static_cast<std::uint32_t>(ranges_.size()) - begin;
        const std::span<const DrawRange> span(ranges_.data() + begin, count);
        while (bi < batchGroup_.size() && batchGroup_[bi] == g) batches_[bi++].ranges = span;
        while (ei < edgeBatchGroup_.size() && edgeBatchGroup_[ei] == g) {
            edgeBatches_[ei++].ranges = span;
        }
    }

    cullStats_.ranges = ranges_.size();
    cullStats_.lastCullMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
            .count();
}

}  // namespace cad::render
