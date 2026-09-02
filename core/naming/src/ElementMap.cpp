#include "cad/naming/ElementMap.h"

#include "cad/kernel/internal/Occt.h"

#include "cad/kernel/Guard.h"

#include <BRep_Tool.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <gp_Pnt.hxx>
#include <TopExp.hxx>
#include <TopoDS.hxx>
#include <TopTools_DataMapOfShapeInteger.hxx>
#include <TopTools_IndexedMapOfShape.hxx>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <vector>
#include <unordered_map>

namespace cad::naming {
namespace {

constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

inline void mix(std::uint64_t& h, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        h ^= (v >> (i * 8)) & 0xFFu;
        h *= kFnvPrime;
    }
}

/// Quantise a coordinate before hashing. Floating-point noise between two builds of the
/// same model would otherwise produce different content hashes and destroy DDC hit rates.
/// 1e-7 mm is far below any meaningful CAD tolerance.
inline std::int64_t quantise(double v) {
    return static_cast<std::int64_t>(std::llround(v * 1e7));
}

}  // namespace

struct ElementMap::Impl {
    struct Entry {
        ElementName name;
        TopoDS_Shape shape;
    };

    std::vector<Entry> entries;
    TopTools_DataMapOfShapeInteger shapeToEntry;          ///< TopoDS_Shape -> entry index
    std::unordered_map<std::uint64_t, std::size_t> byDigest;
    std::unordered_map<std::uint64_t, std::vector<std::size_t>> byFamily;
    std::unordered_map<std::uint64_t, std::uint64_t> aliasToCanonical;
};

ElementMap::ElementMap() : impl_(std::make_unique<Impl>()) {}
ElementMap::~ElementMap() = default;
ElementMap::ElementMap(const ElementMap& o) : impl_(std::make_unique<Impl>(*o.impl_)) {}
ElementMap::ElementMap(ElementMap&&) noexcept = default;
ElementMap& ElementMap::operator=(ElementMap&&) noexcept = default;

ElementMap& ElementMap::operator=(const ElementMap& o) {
    if (this != &o) impl_ = std::make_unique<Impl>(*o.impl_);
    return *this;
}

void ElementMap::bind(const kernel::Shape& sub, ElementName name) {
    if (sub.isNull() || name.isNull()) return;
    const TopoDS_Shape& s = kernel::occt(const_cast<kernel::Shape&>(sub));

    const std::size_t index = impl_->entries.size();
    impl_->entries.push_back({name, s});
    if (!impl_->shapeToEntry.IsBound(s)) {
        impl_->shapeToEntry.Bind(s, static_cast<int>(index));
    }
    impl_->byDigest[name.digest()] = index;
    impl_->byFamily[name.family().digest()].push_back(index);
}

void ElementMap::alias(const ElementName& canonical, ElementName also) {
    if (canonical.isNull() || also.isNull()) return;
    impl_->aliasToCanonical[also.digest()] = canonical.digest();
    // An alias also belongs to the canonical element's family, so resolveAll finds it.
    const auto it = impl_->byDigest.find(canonical.digest());
    if (it != impl_->byDigest.end()) {
        impl_->byFamily[also.family().digest()].push_back(it->second);

        // And nameOf() must agree about which name is canonical.
        //
        // `bind` binds shapeToEntry only on a shape's FIRST binding, so an element bound under two
        // names keeps pointing at whichever arrived first -- which is claim order, i.e. input
        // order. Declaring a different name canonical afterwards left nameOf() answering with the
        // old one, and deriveBoundaries names every edge and vertex from nameOf() of its bounding
        // faces. So the names of a merged shape's edges depended on the order its inputs happened
        // to be passed in, while the canonical rule (lexicographically smallest) is order-free.
        impl_->shapeToEntry.Bind(impl_->entries[it->second].shape, static_cast<int>(it->second));
    }
}

std::optional<ElementName> ElementMap::nameOf(const kernel::Shape& sub) const {
    if (sub.isNull()) return std::nullopt;
    const TopoDS_Shape& s = kernel::occt(const_cast<kernel::Shape&>(sub));
    if (!impl_->shapeToEntry.IsBound(s)) return std::nullopt;
    return impl_->entries[static_cast<std::size_t>(impl_->shapeToEntry.Find(s))].name;
}

std::optional<kernel::Shape> ElementMap::resolve(const ElementName& name) const {
    return resolve(ElementId{name.digest()});
}

std::optional<kernel::Shape> ElementMap::resolve(const ElementId& id) const {
    std::uint64_t digest = id.digest;
    if (const auto a = impl_->aliasToCanonical.find(digest); a != impl_->aliasToCanonical.end()) {
        digest = a->second;
    }
    const auto it = impl_->byDigest.find(digest);
    if (it == impl_->byDigest.end()) return std::nullopt;
    return kernel::wrap(impl_->entries[it->second].shape);
}

std::vector<kernel::Shape> ElementMap::resolveAll(const ElementName& name) const {
    std::vector<kernel::Shape> out;

    // Exact hit first — the common case, and the only one for an unsplit element.
    if (auto exact = resolve(name)) {
        out.push_back(*exact);
        return out;
    }

    // No exact hit: the element may have been split since the reference was taken. Return
    // every child of its family. "Fillet this edge" means the whole edge, even if the edge
    // is now two edges.
    const auto fam = impl_->byFamily.find(name.family().digest());
    if (fam == impl_->byFamily.end()) return out;

    std::vector<std::size_t> indices = fam->second;
    std::sort(indices.begin(), indices.end());
    indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
    // Deterministic output order: by name, never by container order.
    std::sort(indices.begin(), indices.end(), [&](std::size_t a, std::size_t b) {
        return impl_->entries[a].name < impl_->entries[b].name;
    });
    for (std::size_t i : indices) out.push_back(kernel::wrap(impl_->entries[i].shape));
    return out;
}

std::vector<ElementName> ElementMap::allNames() const {
    std::vector<ElementName> out;
    out.reserve(impl_->entries.size());
    for (const auto& e : impl_->entries) out.push_back(e.name);
    std::sort(out.begin(), out.end());
    return out;
}

std::size_t ElementMap::size() const noexcept { return impl_->entries.size(); }

std::uint64_t ElementMap::digest() const noexcept {
    std::vector<std::uint64_t> digests;
    digests.reserve(impl_->entries.size());
    for (const auto& e : impl_->entries) digests.push_back(e.name.digest());
    std::sort(digests.begin(), digests.end());
    std::uint64_t h = kFnvOffset;
    for (auto d : digests) mix(h, d);
    return h;
}

std::vector<ElementName> ElementMap::collisions() const {
    // First binding per digest, then anything that lands on it holding a different element.
    std::unordered_map<std::uint64_t, std::size_t> firstSeen;
    std::unordered_map<std::uint64_t, bool> reported;
    std::vector<ElementName> out;

    for (std::size_t i = 0; i < impl_->entries.size(); ++i) {
        const std::uint64_t digest = impl_->entries[i].name.digest();
        const auto [it, inserted] = firstSeen.emplace(digest, i);
        if (inserted) continue;
        // IsSame, not IsEqual: orientation does not make an element a different element, and a
        // face bound once forward and once reversed is still one face.
        if (impl_->entries[it->second].shape.IsSame(impl_->entries[i].shape)) continue;
        if (!reported[digest]) {
            reported[digest] = true;
            out.push_back(impl_->entries[i].name);
        }
    }
    return out;
}

std::vector<kernel::Shape> ElementMap::unnamed(const kernel::Shape& owner) const {
    std::vector<kernel::Shape> out;
    if (owner.isNull()) return out;
    for (auto type : {kernel::ShapeType::Face, kernel::ShapeType::Edge,
                      kernel::ShapeType::Vertex}) {
        for (const auto& sub : owner.subShapes(type)) {
            if (!impl_->shapeToEntry.IsBound(kernel::occt(const_cast<kernel::Shape&>(sub)))) {
                out.push_back(sub);
            }
        }
    }
    return out;
}

kernel::ShapeHash contentHashUnguarded(const kernel::Shape&, const ElementMap&);

kernel::ShapeHash contentHash(const kernel::Shape& shape, const ElementMap& map) {
    // Guarded, because everything below reaches OCCT -- subShapes walks explorers, and the per
    // element record takes mass properties -- and this is called from the render path and the
    // plugin ABI, neither of which has a Result to carry a failure and neither of which sits under
    // a handler. An unhashable shape used to end the process here.
    auto hashed = kernel::guard("hash a shape's content",
                                [&] { return contentHashUnguarded(shape, map); });
    if (!hashed) {
        kernel::ShapeHash failed;
        failed.valid = false;
        return failed;
    }
    return hashed.value();
}

kernel::ShapeHash contentHashUnguarded(const kernel::Shape& shape, const ElementMap& map) {
    kernel::ShapeHash out;
    if (shape.isNull()) return out;

    // Every element, visited in ELEMENT-NAME order, never in OCCT traversal order. That ordering is
    // what makes the hash identical across processes and machines, which is the whole basis for the
    // DDC's shared tier being worth anything.
    //
    // # What this has to describe
    //
    // It used to describe faces only, and each face only by its area and centroid. Two shapes with
    // the same face names, areas and centroids therefore hashed identically however differently
    // those faces were arranged, whatever curves bounded them, and whatever surfaces they lay on --
    // and edges and vertices contributed nothing at all.
    //
    // That mattered in two places. `serialForShapes` in the C ABI derives an operation's naming
    // serial from its inputs, so two inputs that hash alike mint the same names for different
    // geometry. And every determinism check in the project -- the cross-process test, the ABI's
    // run-it-twice self-check -- asserts "same hash, same result", which is only as strong as the
    // description underneath it. A check that cannot see topology passes on two shapes that differ
    // in topology.
    //
    // Note what this is NOT: it is not the recompute cache key. The DDC keys on
    // Engine::cacheKeyOf, which is derived from a feature's inputs -- type, version, properties,
    // upstream keys -- so a collision here has never been able to serve the wrong cached geometry.
    struct Record {
        ElementName name;
        std::array<std::int64_t, 6> values{};
    };
    std::vector<Record> records;

    const auto recordOf = [&map](const kernel::Shape& element) -> std::optional<Record> {
        auto name = map.nameOf(element);
        if (!name) return std::nullopt;   // unnamed elements are reported separately
        const TopoDS_Shape& raw = kernel::occt(const_cast<kernel::Shape&>(element));

        Record record;
        record.name = *name;
        // The TYPE. Records are keyed by NAME and a name is unique within a shape, so this only
        // earns its place ACROSS two shapes, where one name can sit on a face in the first and on
        // an edge in the second whose length happens to quantise to the same value as the face's
        // area. Kept because it is one line and clearly right; not covered by a test, because
        // constructing that coincidence is contrived enough that the test would be asserting its
        // own setup.
        record.values[0] = static_cast<std::int64_t>(raw.ShapeType());
        record.values[1] = static_cast<std::int64_t>(raw.Orientation());

        if (raw.ShapeType() == TopAbs_VERTEX) {
            // Largely redundant while edges are hashed -- an edge determines its own endpoints --
            // and not redundant for a shape that is ONLY points, which a sketch can be.
            const gp_Pnt p = BRep_Tool::Pnt(TopoDS::Vertex(raw));
            record.values[2] = 0;
            record.values[3] = quantise(p.X());
            record.values[4] = quantise(p.Y());
            record.values[5] = quantise(p.Z());
            return record;
        }

        GProp_GProps props;
        if (raw.ShapeType() == TopAbs_EDGE) {
            BRepGProp::LinearProperties(raw, props);   // length, not area
        } else {
            BRepGProp::SurfaceProperties(raw, props);
        }
        const gp_Pnt c = props.CentreOfMass();
        record.values[2] = quantise(props.Mass());
        record.values[3] = quantise(c.X());
        record.values[4] = quantise(c.Y());
        record.values[5] = quantise(c.Z());
        return record;
    };

    // Faces, then edges, then vertices. Edges and vertices carry the shape's STRUCTURE: two
    // arrangements of the same faces differ in which curves bound them and where those curves meet,
    // and nothing above the face level can see that.
    for (const auto type : {kernel::ShapeType::Face, kernel::ShapeType::Edge,
                            kernel::ShapeType::Vertex}) {
        for (const auto& element : shape.subShapes(type)) {
            if (auto record = recordOf(element)) records.push_back(std::move(*record));
        }
    }
    std::sort(records.begin(), records.end(),
              [](const Record& a, const Record& b) { return a.name < b.name; });

    std::uint64_t lanes[4] = {kFnvOffset, kFnvOffset ^ 0x1111111111111111ULL,
                              kFnvOffset ^ 0x2222222222222222ULL,
                              kFnvOffset ^ 0x3333333333333333ULL};

    // What the shape IS, and how it is put together, before anything about its elements.
    //
    // A solid and a compound holding that solid's own faces have identical faces, identical edges
    // and identical vertices -- `subShapes` reaches through either one to the same elements. They
    // are still completely different things: one is a closed body with a volume, the other is a bag
    // of surfaces. The difference lives entirely in the levels ABOVE a face, so the root's own type
    // and the counts of solids, shells and wires are what separate them.
    mix(lanes[0], static_cast<std::uint64_t>(
                      kernel::occt(const_cast<kernel::Shape&>(shape)).ShapeType()));

    // The COUNTS, per level. Nearly free, and they also separate two shapes that differ only by an
    // element nobody could name -- which the record loop skips and would otherwise say nothing
    // about at all.
    for (const auto type : {kernel::ShapeType::CompSolid, kernel::ShapeType::Solid,
                            kernel::ShapeType::Shell, kernel::ShapeType::Face,
                            kernel::ShapeType::Wire, kernel::ShapeType::Edge,
                            kernel::ShapeType::Vertex}) {
        mix(lanes[0], static_cast<std::uint64_t>(shape.subShapes(type).size()));
    }
    mix(lanes[0], static_cast<std::uint64_t>(records.size()));

    for (const auto& record : records) {
        mix(lanes[0], record.name.digest());
        mix(lanes[1], static_cast<std::uint64_t>(record.values[0]));
        mix(lanes[1], static_cast<std::uint64_t>(record.values[1]));
        mix(lanes[1], static_cast<std::uint64_t>(record.values[2]));
        mix(lanes[2], static_cast<std::uint64_t>(record.values[3])
                          ^ static_cast<std::uint64_t>(record.values[4]));
        mix(lanes[3], static_cast<std::uint64_t>(record.values[5]));
    }
    // ADR 0005: the element map participates in the content hash.
    mix(lanes[0], map.digest());

    for (int i = 0; i < 4; ++i) out.lanes[i] = lanes[i];
    return out;
}

}  // namespace cad::naming
