#include "cad/naming/ElementMap.h"

#include "cad/kernel/internal/Occt.h"

#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <gp_Pnt.hxx>
#include <TopExp.hxx>
#include <TopTools_DataMapOfShapeInteger.hxx>
#include <TopTools_IndexedMapOfShape.hxx>

#include <algorithm>
#include <cmath>
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

kernel::ShapeHash contentHash(const kernel::Shape& shape, const ElementMap& map) {
    kernel::ShapeHash out;
    if (shape.isNull()) return out;

    // Visit faces in ELEMENT-NAME order, never in OCCT traversal order. This is what makes
    // the hash identical across processes and machines, which is the whole basis for the
    // DDC's shared tier being worth anything.
    struct FaceRec {
        ElementName name;
        std::int64_t area;
        std::int64_t cx, cy, cz;
    };
    std::vector<FaceRec> faces;

    for (const auto& f : shape.subShapes(kernel::ShapeType::Face)) {
        auto name = map.nameOf(f);
        if (!name) continue;  // unnamed faces are reported separately as NamingLost
        GProp_GProps props;
        BRepGProp::SurfaceProperties(kernel::occt(const_cast<kernel::Shape&>(f)), props);
        const gp_Pnt c = props.CentreOfMass();
        faces.push_back({*name, quantise(props.Mass()),
                         quantise(c.X()), quantise(c.Y()), quantise(c.Z())});
    }
    std::sort(faces.begin(), faces.end(),
              [](const FaceRec& a, const FaceRec& b) { return a.name < b.name; });

    std::uint64_t lanes[4] = {kFnvOffset, kFnvOffset ^ 0x1111111111111111ULL,
                              kFnvOffset ^ 0x2222222222222222ULL,
                              kFnvOffset ^ 0x3333333333333333ULL};
    for (const auto& f : faces) {
        mix(lanes[0], f.name.digest());
        mix(lanes[1], static_cast<std::uint64_t>(f.area));
        mix(lanes[2], static_cast<std::uint64_t>(f.cx) ^ static_cast<std::uint64_t>(f.cy));
        mix(lanes[3], static_cast<std::uint64_t>(f.cz));
    }
    // ADR 0005: the element map participates in the content hash.
    mix(lanes[0], map.digest());

    for (int i = 0; i < 4; ++i) out.lanes[i] = lanes[i];
    return out;
}

}  // namespace cad::naming
