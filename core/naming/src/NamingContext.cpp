#include "cad/naming/ElementMap.h"

#include "cad/kernel/Guard.h"
#include "cad/kernel/internal/Occt.h"

#include <BRep_Tool.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <TopoDS.hxx>
#include <gp_Pnt.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <BRepTools_History.hxx>

#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>
#include <unordered_map>

namespace cad::naming {
namespace {

inline std::int64_t q(double v) { return static_cast<std::int64_t>(std::llround(v * 1e7)); }

struct FaceMetric {
    std::int64_t area, cx, cy, cz;
    bool operator==(const FaceMetric&) const = default;
    bool operator<(const FaceMetric& o) const {
        return std::tie(area, cx, cy, cz) < std::tie(o.area, o.cx, o.cy, o.cz);
    }
};

FaceMetric measureFace(const TopoDS_Shape& f) {
    GProp_GProps p;
    BRepGProp::SurfaceProperties(f, p);
    const gp_Pnt c = p.CentreOfMass();
    return {q(p.Mass()), q(c.X()), q(c.Y()), q(c.Z())};
}

struct Point3 {
    std::int64_t x, y, z;
    bool operator<(const Point3& o) const {
        return std::tie(x, y, z) < std::tie(o.x, o.y, o.z);
    }
};

Point3 midpointOf(const TopoDS_Shape& s) {
    // A vertex has no mass, so GProp would report the origin for every one of them and the
    // sibling ordering would collapse. Read the point directly.
    if (s.ShapeType() == TopAbs_VERTEX) {
        const gp_Pnt c = BRep_Tool::Pnt(TopoDS::Vertex(s));
        return {q(c.X()), q(c.Y()), q(c.Z())};
    }
    GProp_GProps p;
    if (s.ShapeType() == TopAbs_EDGE) {
        BRepGProp::LinearProperties(s, p);
    } else {
        BRepGProp::SurfaceProperties(s, p);
    }
    const gp_Pnt c = p.CentreOfMass();
    return {q(c.X()), q(c.Y()), q(c.Z())};
}

}  // namespace

struct NamingContext::Impl {
    std::uint32_t featureSerial;
    std::uint16_t opTag;

    /// Names every edge and vertex of `result` by the set of faces it bounds.
    ///
    /// This is the load-bearing step. Face identity is tracked through operations by the
    /// algorithm's own reports (which OCCT does reasonably well); everything of lower
    /// dimension is then derived, so it inherits that stability for free. Change a box's
    /// width and the edge {Top, Front} is still the edge {Top, Front}.
    kernel::Result<void> deriveBoundaries(const kernel::Shape& result, ElementMap& map) const {
        const TopoDS_Shape& shape = kernel::occt(const_cast<kernel::Shape&>(result));

        for (const auto subType : {TopAbs_EDGE, TopAbs_VERTEX}) {
            TopTools_IndexedDataMapOfShapeListOfShape ancestors;
            TopExp::MapShapesAndAncestors(shape, subType, TopAbs_FACE, ancestors);

            // Group by bounding-face-name set so siblings can be discriminated.
            std::map<std::vector<std::uint64_t>, std::vector<TopoDS_Shape>> byBoundary;

            for (int i = 1; i <= ancestors.Extent(); ++i) {
                const TopoDS_Shape& sub = ancestors.FindKey(i);
                std::vector<std::uint64_t> parents;
                bool complete = true;
                for (const auto& face : ancestors.FindFromIndex(i)) {
                    auto n = map.nameOf(kernel::wrap(face));
                    if (!n) { complete = false; break; }
                    parents.push_back(n->digest());
                }
                if (!complete || parents.empty()) {
                    // A bounding face we could not name. Do not invent a name for the edge —
                    // it would be a plausible-looking lie. Leave it unnamed so
                    // ElementMap::unnamed reports it and the caller raises NamingLost.
                    continue;
                }
                std::sort(parents.begin(), parents.end());
                parents.erase(std::unique(parents.begin(), parents.end()), parents.end());
                byBoundary[parents].push_back(sub);
            }

            for (auto& [parents, siblings] : byBoundary) {
                // Siblings share a boundary set — an edge split in two, or the two edges a
                // cylinder's seam produces. Order them canonically by midpoint.
                //
                // THIS IS THE FRAGILE PART OF THE SCHEME (ADR 0005). Midpoint ordering is
                // stable under the parameter changes we care about but is not stable in
                // general: a change that swaps two siblings' positions swaps their
                // discriminators. When that matters we will need a stronger invariant.
                std::sort(siblings.begin(), siblings.end(),
                          [](const TopoDS_Shape& a, const TopoDS_Shape& b) {
                              return midpointOf(a) < midpointOf(b);
                          });

                const bool needsDiscriminator = siblings.size() > 1;
                for (std::size_t k = 0; k < siblings.size(); ++k) {
                    NameStep step;
                    // featureSerial/opTag are deliberately left at 0 for boundary names.
                    //
                    // A boundary name is a pure function of the bounding faces' names, so
                    // it must NOT be attributed to whichever feature happened to rebuild
                    // the shape. Stamping the current feature here would give the edge
                    // {Top, Front} a different name in the box than in the fillet result,
                    // and every downstream reference would break on the next operation —
                    // which is precisely the bug this whole layer exists to prevent.
                    step.provenance = Provenance::Boundary;
                    step.parents = parents;
                    step.discriminator = needsDiscriminator ? static_cast<std::uint32_t>(k + 1) : 0;
                    map.bind(kernel::wrap(siblings[k]), ElementName({step}));
                }
            }
        }
        return {};
    }
};

NamingContext::NamingContext(std::uint32_t featureSerial, std::uint16_t opTag)
    : impl_(std::make_unique<Impl>(Impl{featureSerial, opTag})) {}

NamingContext::~NamingContext() = default;

kernel::Result<ElementMap> NamingContext::nameprimitive(
    const kernel::Shape& result, const std::vector<kernel::Shape>& taggedFaces) {
    if (result.isNull()) {
        return kernel::Error{kernel::ErrorCode::InvalidInput, "Cannot name an empty shape."};
    }

    ElementMap map;

    if (!taggedFaces.empty()) {
        // Tags are positional and come from the primitive's own constructor
        // (BRepPrimAPI_MakeBox::TopFace() and friends), never from explorer order.
        for (std::size_t i = 0; i < taggedFaces.size(); ++i) {
            if (taggedFaces[i].isNull()) continue;
            NameStep step;
            step.featureSerial = impl_->featureSerial;
            step.opTag = impl_->opTag;
            step.provenance = Provenance::Primitive;
            step.discriminator = static_cast<std::uint32_t>(i);
            map.bind(taggedFaces[i], ElementName({step}));
        }
    } else {
        // No constructor tags: a cylinder, or geometry read from a foreign file. Name every
        // face by its position in a canonical MEASURE order - area then centroid, both
        // quantised - which is deterministic across processes and machines.
        //
        // The honest caveat: unlike a box's positional tags, these names are only as stable
        // as the geometry itself. Change a cylinder's radius and the faces keep their names
        // (their relative measure order is unchanged); change it enough to reorder two faces
        // by area and they swap. For imported foreign geometry there is nothing better
        // available - the file carries no construction history - and a deterministic name is
        // still far more useful than an explorer index, which changes on every read.
        //
        // Primitives that CAN expose tagged faces should; see the note in computeCylinder.
        std::vector<std::pair<FaceMetric, TopoDS_Shape>> faces;
        for (const auto& f : result.subShapes(kernel::ShapeType::Face)) {
            const TopoDS_Shape& of = kernel::occt(const_cast<kernel::Shape&>(f));
            faces.emplace_back(measureFace(of), of);
        }
        std::sort(faces.begin(), faces.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });

        for (std::size_t i = 0; i < faces.size(); ++i) {
            NameStep step;
            step.featureSerial = impl_->featureSerial;
            step.opTag = impl_->opTag;
            step.provenance = Provenance::Primitive;
            step.discriminator = static_cast<std::uint32_t>(i);
            map.bind(kernel::wrap(faces[i].second), ElementName({step}));
        }
    }

    if (auto r = impl_->deriveBoundaries(result, map); !r) return r.error();

    // Curve-only geometry: a sketch whose lines do not close has no faces at all, so the loop
    // above named nothing and deriveBoundaries had nothing to derive FROM. Naming the edges
    // directly is the same idea one level down — sorted by their own measure, so the names are
    // deterministic and survive a re-solve that does not change the geometry.
    //
    // Without this an open sketch is NamingLost, which is how "draw one line" became an error.
    if (result.subShapes(kernel::ShapeType::Face).empty()) {
        std::vector<std::pair<FaceMetric, TopoDS_Shape>> curves;
        for (const auto& e : result.subShapes(kernel::ShapeType::Edge)) {
            const TopoDS_Shape& oe = kernel::occt(const_cast<kernel::Shape&>(e));
            curves.emplace_back(measureFace(oe), oe);
        }
        std::sort(curves.begin(), curves.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        for (std::size_t i = 0; i < curves.size(); ++i) {
            NameStep step;
            step.featureSerial = impl_->featureSerial;
            step.opTag = impl_->opTag;
            step.provenance = Provenance::Primitive;
            // Offset past the face discriminators so an edge can never collide with a face name
            // in a shape that has both.
            step.discriminator = static_cast<std::uint32_t>(i) + 0x40000000u;
            map.bind(kernel::wrap(curves[i].second), ElementName({step}));
        }
        // Vertices too, and directly: deriveBoundaries walks a face's boundary, so with no faces
        // it has nothing to walk and every endpoint would stay unnamed.
        std::vector<std::pair<FaceMetric, TopoDS_Shape>> points;
        for (const auto& v : result.subShapes(kernel::ShapeType::Vertex)) {
            const TopoDS_Shape& ov = kernel::occt(const_cast<kernel::Shape&>(v));
            points.emplace_back(measureFace(ov), ov);
        }
        std::sort(points.begin(), points.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        for (std::size_t i = 0; i < points.size(); ++i) {
            NameStep step;
            step.featureSerial = impl_->featureSerial;
            step.opTag = impl_->opTag;
            step.provenance = Provenance::Primitive;
            step.discriminator = static_cast<std::uint32_t>(i) + 0x60000000u;
            map.bind(kernel::wrap(points[i].second), ElementName({step}));
        }
    }

    if (const auto missing = map.unnamed(result); !missing.empty()) {
        return kernel::Error{kernel::ErrorCode::NamingLost,
                             "Some geometry could not be identified.",
                             std::to_string(missing.size()) +
                                 " unnamed sub-elements after naming a primitive"};
    }
    return map;
}

kernel::Result<ElementMap> NamingContext::propagate(
    const kernel::Operation& op,
    const std::vector<const kernel::Shape*>& inputs,
    const std::vector<const ElementMap*>& inputMaps) {

    if (inputs.size() != inputMaps.size()) {
        return kernel::Error{kernel::ErrorCode::InvalidInput,
                             "Naming: input and map counts differ."};
    }
    const kernel::Shape result = op.shape();
    if (result.isNull()) {
        return kernel::Error{kernel::ErrorCode::InvalidInput,
                             "Naming: operation produced no shape."};
    }

    auto* algo = op.impl().algo.get();
    const Handle(BRepTools_History)& history = op.impl().history;
    const bool haveReports = (algo != nullptr) || !history.IsNull();

    // Uniform access to whichever reporting mechanism this algorithm offers.
    // BRepBuilderAPI_MakeShape and BRepTools_History carry the same information behind
    // different names (IsDeleted vs IsRemoved); callers should not have to care.
    static const TopTools_ListOfShape kEmpty;
    const auto reportedModified = [&](const TopoDS_Shape& s) -> const TopTools_ListOfShape& {
        if (algo) return algo->Modified(s);
        if (!history.IsNull() && history->IsSupportedType(s)) return history->Modified(s);
        return kEmpty;
    };
    const auto reportedGenerated = [&](const TopoDS_Shape& s) -> const TopTools_ListOfShape& {
        if (algo) return algo->Generated(s);
        if (!history.IsNull() && history->IsSupportedType(s)) return history->Generated(s);
        return kEmpty;
    };
    const auto reportedGone = [&](const TopoDS_Shape& s) -> bool {
        if (algo) return algo->IsDeleted(s) == Standard_True;
        if (!history.IsNull() && history->IsSupportedType(s)) {
            return history->IsRemoved(s) == Standard_True;
        }
        return false;
    };

    ElementMap out;

    // Result faces claimed so far, keyed by the face itself. A second claimant is a merge,
    // not an overwrite.
    std::map<FaceMetric, ElementName> claimed;

    const auto claim = [&](const TopoDS_Shape& face, ElementName name) {
        const FaceMetric key = measureFace(face);
        const auto it = claimed.find(key);
        if (it == claimed.end()) {
            claimed.emplace(key, name);
            out.bind(kernel::wrap(face), std::move(name));
            return;
        }
        // Two input faces landed on one result face: a merge (two coplanar faces unified,
        // say). BOTH names must resolve to the face — a user reference to either one has to
        // survive — so bind the newcomer as well, and record which name is canonical.
        //
        // Binding is what makes resolution work; the alias entry only records canonicity.
        // Recording the alias without binding would leave the smaller name unresolvable,
        // which is the subtle version of exactly the bug this layer exists to prevent.
        out.bind(kernel::wrap(face), name);
        if (name < it->second) {
            out.alias(name, it->second);
            it->second = name;
        } else {
            out.alias(it->second, name);
        }
    };

    auto resultFaces = result.subShapes(kernel::ShapeType::Face);

    // --- 1. the algorithm's own reports -------------------------------------------------
    if (haveReports) {
        for (std::size_t i = 0; i < inputs.size(); ++i) {
            const kernel::Shape& in = *inputs[i];
            const ElementMap& inMap = *inputMaps[i];
            if (in.isNull()) continue;

            for (const auto& face : in.subShapes(kernel::ShapeType::Face)) {
                const auto name = inMap.nameOf(face);
                if (!name) continue;
                TopoDS_Shape& of = kernel::occt(const_cast<kernel::Shape&>(face));

                if (reportedGone(of)) continue;  // gone; references will fail to resolve

                bool reported = false;
                for (const auto& m : reportedModified(of)) {
                    if (m.ShapeType() != TopAbs_FACE) continue;
                    claim(m, *name);   // same element, altered: keep the name
                    reported = true;
                }
                for (const auto& g : reportedGenerated(of)) {
                    if (g.ShapeType() != TopAbs_FACE) continue;
                    NameStep s{impl_->featureSerial, impl_->opTag, Provenance::Generated, 0,
                               {name->digest()}};
                    claim(g, name->derive(s));
                    reported = true;
                }
                if (!reported) {
                    // Untouched face carried straight through.
                    claim(of, *name);
                }
            }

            // Fillets generate their faces FROM EDGES, so edges must be interrogated too.
            // Missing this is the classic reason a filleted face ends up anonymous.
            for (const auto& edge : in.subShapes(kernel::ShapeType::Edge)) {
                const auto name = inMap.nameOf(edge);
                if (!name) continue;
                TopoDS_Shape& oe = kernel::occt(const_cast<kernel::Shape&>(edge));
                for (const auto& g : reportedGenerated(oe)) {
                    if (g.ShapeType() != TopAbs_FACE) continue;
                    NameStep s{impl_->featureSerial, impl_->opTag, Provenance::Generated, 0,
                               {name->digest()}};
                    claim(g, name->derive(s));
                }
            }
        }
    }

    // --- 2. geometric fallback ----------------------------------------------------------
    // OCCT's Generated/Modified coverage is genuinely incomplete — notably for booleans and
    // for algorithms like ShapeUpgrade_UnifySameDomain that are not MakeShape at all. Match
    // leftover result faces against input faces by measure. This is not a nicety; without it
    // ordinary boolean results come out partly anonymous.
    std::map<FaceMetric, ElementName> inputByMetric;
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        if (inputs[i]->isNull()) continue;
        for (const auto& face : inputs[i]->subShapes(kernel::ShapeType::Face)) {
            if (const auto n = inputMaps[i]->nameOf(face)) {
                inputByMetric.emplace(measureFace(kernel::occt(const_cast<kernel::Shape&>(face))),
                                      *n);
            }
        }
    }
    for (const auto& face : resultFaces) {
        if (out.nameOf(face)) continue;
        const TopoDS_Shape& of = kernel::occt(const_cast<kernel::Shape&>(face));
        const auto it = inputByMetric.find(measureFace(of));
        if (it != inputByMetric.end()) {
            claim(of, it->second);
        }
    }

    // --- 3. genuinely new faces ---------------------------------------------------------
    // A cut's tool faces, for instance. They belong to this feature and are tagged by their
    // own measure so the tag is reproducible, not by iteration index.
    {
        std::vector<std::pair<FaceMetric, TopoDS_Shape>> newFaces;
        for (const auto& face : resultFaces) {
            if (out.nameOf(face)) continue;
            const TopoDS_Shape& of = kernel::occt(const_cast<kernel::Shape&>(face));
            newFaces.emplace_back(measureFace(of), of);
        }
        std::sort(newFaces.begin(), newFaces.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        for (std::size_t k = 0; k < newFaces.size(); ++k) {
            NameStep s;
            s.featureSerial = impl_->featureSerial;
            s.opTag = impl_->opTag;
            s.provenance = Provenance::Generated;
            s.discriminator = static_cast<std::uint32_t>(k + 1);
            out.bind(kernel::wrap(newFaces[k].second), ElementName({s}));
        }
    }

    // --- 4. edges and vertices by boundary ----------------------------------------------
    if (auto r = impl_->deriveBoundaries(result, out); !r) return r.error();

    if (const auto missing = out.unnamed(result); !missing.empty()) {
        return kernel::Error{kernel::ErrorCode::NamingLost,
                             "Some geometry could not be identified after this operation.",
                             std::to_string(missing.size()) + " unnamed sub-elements"};
    }
    return out;
}

}  // namespace cad::naming
