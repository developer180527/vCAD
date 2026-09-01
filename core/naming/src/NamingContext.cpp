#include "cad/naming/ElementMap.h"

#include "cad/kernel/Guard.h"
#include "cad/kernel/internal/Occt.h"

#include "ClaimResolver.h"
#include "Measure.h"

#include <BRep_Tool.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <TopoDS.hxx>
#include <gp_Pnt.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopTools_MapOfShape.hxx>
#include <BRepTools_History.hxx>

#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>
#include <unordered_map>

namespace cad::naming {

// The measurement vocabulary lives in Measure.h now: five places need it, and one of them is a
// separate translation unit.
using internal::FaceMetric;
using internal::measureFace;
using internal::midpointOf;
using internal::Point3;
using internal::quantise;

namespace {

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
                // THIS IS THE FRAGILE PART OF THE SCHEME (ADR 0005). The ordering is stable under
                // the parameter changes we care about but is not stable in general: a change that
                // swaps two siblings' positions swaps their discriminators.
                //
                // The key is boundaryKeyOf rather than a bare midpoint -- midpoint FIRST, then
                // length, endpoints and curve type. Wherever midpoints already differ the extra
                // terms are never reached and the order is exactly what it was, so no saved name
                // moves; they only decide siblings that a midpoint alone left tied, which used to
                // be refused outright. What remains genuinely undecidable is still refused, because
                // numbering by unspecified sort order is the wrong kind of answer.
                const bool needsDiscriminator = siblings.size() > 1;
                // The size is read BEFORE the move. Written as one braced initialiser with
                // `std::move(siblings)` alongside `siblings.size()`, the order of evaluation is
                // unspecified: the move can empty the vector first, leaving the flag array shorter
                // than the element array and every later index out of bounds. It segfaulted.
                const std::size_t siblingCount = siblings.size();
                const auto ordered =
                    needsDiscriminator
                        ? internal::canonicalOrder(std::move(siblings), internal::boundaryKeyOf)
                        : internal::CanonicalOrder{std::move(siblings),
                                                   std::vector<std::uint8_t>(siblingCount)};
                siblings = ordered.elements;
                for (std::size_t k = 0; k < siblings.size(); ++k) {
                    if (ordered.ambiguous[k] != 0) continue;
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
                    // The TOPOLOGY TYPE is folded into the discriminator, and it has to be.
                    //
                    // A boundary name is a function of the bounding faces' names — and for a lone
                    // FACE, such as a sketch profile, an edge and a vertex have the SAME single
                    // bounding face, so both passes produced identical names and the vertex
                    // overwrote the edge. Every edge of every sketch was therefore unreachable: a
                    // revolve could not name its axis, and a fillet could not name a sketch edge.
                    //
                    // Masked on solids, which is why it survived: there an edge has two parent faces
                    // and a vertex three, so their parent sets already differ.
                    //
                    // The VERTEX takes the offset, and which side gets it matters for
                    // compatibility. Edge names on SOLIDS never collided and are referenced
                    // everywhere — every fillet and chamfer resolves one — so moving them would
                    // rename existing references in saved documents. Offsetting the edge instead
                    // broke 67 assertions in m1_naming_stability, which is the check that exists to
                    // catch exactly this. Vertices are referenced by no feature today, so they are
                    // the safe side to move.
                    const std::uint32_t typeBase = subType == TopAbs_EDGE ? 0u : 0x40000000u;
                    step.discriminator =
                        typeBase + (needsDiscriminator ? static_cast<std::uint32_t>(k + 1) : 0u);
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
    const kernel::Shape& result, const std::vector<kernel::Shape>& taggedFaces, Naming naming) {
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
        std::vector<TopoDS_Shape> faces;
        for (const auto& f : result.subShapes(kernel::ShapeType::Face)) {
            faces.push_back(kernel::occt(const_cast<kernel::Shape&>(f)));
        }
        // Ambiguous leaves them unnamed, which `unnamed()` below turns into NamingLost. Two faces
        // of one area and one centroid cannot be told apart, and a name that says otherwise is a
        // reference that will one day resolve to the wrong one.
        const auto ordered = internal::canonicalOrder(std::move(faces), internal::measureFace);
        for (std::size_t i = 0; i < ordered.elements.size(); ++i) {
            // Tied faces are skipped, not numbered. They stay unnamed, which `unnamed()` reports --
            // strictly for geometry we built, best-effort for geometry we read (see the Naming
            // parameter). The others keep their positions, so one duplicate pair in an imported
            // file costs only itself.
            if (ordered.ambiguous[i] != 0) continue;
            NameStep step;
            step.featureSerial = impl_->featureSerial;
            step.opTag = impl_->opTag;
            step.provenance = Provenance::Primitive;
            step.discriminator = static_cast<std::uint32_t>(i);
            map.bind(kernel::wrap(ordered.elements[i]), ElementName({step}));
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
        // MIDPOINT, not measureFace. measureFace reads SURFACE properties, which are zero for an
        // edge and zero for a vertex -- so every curve in an open sketch measured (0,0,0,0), the
        // sort was a no-op, and the discriminator was the element's position in OCCT's traversal
        // order. That is an index-based identity wearing a derivation chain's clothes, and it is
        // exactly what ADR 0005 exists to abolish: reorder the sketch and every reference into it
        // moves, silently. A revolve names its axis this way.
        std::vector<TopoDS_Shape> curveShapes;
        for (const auto& e : result.subShapes(kernel::ShapeType::Edge)) {
            curveShapes.push_back(kernel::occt(const_cast<kernel::Shape&>(e)));
        }
        const auto curves = internal::canonicalOrder(std::move(curveShapes), internal::midpointOf);
        for (std::size_t i = 0; i < curves.elements.size(); ++i) {
            if (curves.ambiguous[i] != 0) continue;
            NameStep step;
            step.featureSerial = impl_->featureSerial;
            step.opTag = impl_->opTag;
            step.provenance = Provenance::Primitive;
            // Offset past the face discriminators so an edge can never collide with a face name
            // in a shape that has both.
            step.discriminator = static_cast<std::uint32_t>(i) + 0x40000000u;
            map.bind(kernel::wrap(curves.elements[i]), ElementName({step}));
        }
        // Vertices too, and directly: deriveBoundaries walks a face's boundary, so with no faces
        // it has nothing to walk and every endpoint would stay unnamed.
        // Vertices by the EDGES THEY BOUND, not by position.
        //
        // Position alone cannot name them. `toEdges` produces a compound of independent curves, so
        // a three-segment chain has six vertices, not four: each joint carries two coincident
        // endpoints, one per edge. They are topologically distinct and geometrically identical, so
        // any ordering by coordinate ties and nothing can be numbered.
        //
        // Which is the same answer the rest of the scheme already gives one dimension up: an edge
        // of a solid is named by the faces it bounds. Here a vertex is named by the edges it
        // bounds, so the two endpoints at a joint differ because they belong to different curves.
        TopTools_IndexedDataMapOfShapeListOfShape vertexOwners;
        TopExp::MapShapesAndAncestors(kernel::occt(const_cast<kernel::Shape&>(result)),
                                      TopAbs_VERTEX, TopAbs_EDGE, vertexOwners);

        std::map<std::vector<std::uint64_t>, std::vector<TopoDS_Shape>> byOwners;
        for (const auto& v : result.subShapes(kernel::ShapeType::Vertex)) {
            const TopoDS_Shape& ov = kernel::occt(const_cast<kernel::Shape&>(v));
            std::vector<std::uint64_t> owners;
            if (vertexOwners.Contains(ov)) {
                for (const auto& e : vertexOwners.FindFromKey(ov)) {
                    if (const auto n = map.nameOf(kernel::wrap(e))) owners.push_back(n->digest());
                }
            }
            if (owners.empty()) continue;   // no named edge to derive from; left for `unnamed()`
            std::sort(owners.begin(), owners.end());
            owners.erase(std::unique(owners.begin(), owners.end()), owners.end());
            byOwners[owners].push_back(ov);
        }

        for (auto& [owners, siblings] : byOwners) {
            // Two endpoints of one curve share its owner set, so they still need separating -- and
            // those two are at genuinely different positions, so a midpoint tells them apart.
            const auto ordered =
                siblings.size() > 1
                    ? internal::canonicalOrder(siblings, internal::midpointOf)
                    : internal::CanonicalOrder{siblings,
                                               std::vector<std::uint8_t>(siblings.size())};
            for (std::size_t i = 0; i < ordered.elements.size(); ++i) {
                if (ordered.ambiguous[i] != 0) continue;
                NameStep step;
                step.provenance = Provenance::Boundary;
                step.parents = owners;
                step.discriminator =
                    (ordered.elements.size() > 1 ? static_cast<std::uint32_t>(i + 1) : 0u)
                    + 0x60000000u;
                map.bind(kernel::wrap(ordered.elements[i]), ElementName({step}));
            }
        }
    }

    // Under BestEffort an unnamed element is a fact about the file, not a failure of the call.
    // The collision check below still applies: two elements sharing one name would be a bug here,
    // not a property of the geometry, and it is never acceptable.
    if (const auto missing = map.unnamed(result);
        !missing.empty() && naming == Naming::Strict) {
        return kernel::Error{kernel::ErrorCode::NamingLost,
                             "Some geometry could not be identified.",
                             std::to_string(missing.size())
                                 + " unnamed sub-elements after naming a primitive; shape has "
                                 + std::to_string(result.subShapes(kernel::ShapeType::Face).size())
                                 + " faces, "
                                 + std::to_string(result.subShapes(kernel::ShapeType::Edge).size())
                                 + " edges, "
                                 + std::to_string(
                                       result.subShapes(kernel::ShapeType::Vertex).size())
                                 + " vertices; first unnamed is type "
                                 + std::to_string(static_cast<int>(missing.front().type()))};
    }
    // A primitive should never collide with itself, so this is a check on the tagging rather than
    // on the geometry: two faces given one tag would otherwise be indistinguishable for good.
    if (const auto ambiguous = map.collisions(); !ambiguous.empty()) {
        return kernel::Error{kernel::ErrorCode::NamingLost,
                             "Two faces of this shape ended up with the same identity.",
                             std::to_string(ambiguous.size()) + " colliding names, first: "
                                 + ambiguous.front().toString()};
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

    // Claims are COLLECTED, not bound as they arrive.
    //
    // Two different things can happen to a face, and they need opposite treatment:
    //
    //   * a MERGE -- two input faces land on one result face. Both names must resolve, so both are
    //     bound and one is recorded as the alias of the other.
    //   * a SPLIT -- one input face becomes several result faces, all reported as `Modified` of the
    //     same parent. They are different elements and must get different names.
    //
    // Binding on arrival can only see the first of these. The second needs every claimant of a name
    // in hand before any of them can be named, because the discriminator depends on how many there
    // are and on their canonical order. Splits were therefore silently unnameable: `bind` kept the
    // last claimant and the earlier faces became unreachable, with `unnamed()` reporting nothing
    // wrong because every face did have a name. Fusing two overlapping boxes lost eight elements
    // that way, and had done since booleans existed.
    std::vector<internal::Claim> claims;
    TopTools_MapOfShape claimedFaces;   ///< so pass 2 can ask what is spoken for before any binding
    const auto claim = [&](const TopoDS_Shape& face, ElementName name, const TopoDS_Shape& parent) {
        claimedFaces.Add(face);
        claims.push_back(internal::Claim{face, std::move(name), parent});
    };

    // Claims are settled in one pass, by resolveClaims -- see ClaimResolver.h for why that is a
    // separate thing rather than a lambda here.

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
                    claim(m, *name, of);   // same element, altered: keep the name
                    reported = true;
                }
                for (const auto& g : reportedGenerated(of)) {
                    if (g.ShapeType() != TopAbs_FACE) continue;
                    NameStep s{impl_->featureSerial, impl_->opTag, Provenance::Generated, 0,
                               {name->digest()}};
                    claim(g, name->derive(s), of);
                    reported = true;
                }
                if (!reported) {
                    // Untouched face carried straight through.
                    claim(of, *name, of);
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
                    claim(g, name->derive(s), oe);
                }
            }
        }
    }

    // --- 2. geometric fallback ----------------------------------------------------------
    // OCCT's Generated/Modified coverage is genuinely incomplete — notably for booleans and
    // for algorithms like ShapeUpgrade_UnifySameDomain that are not MakeShape at all. Match
    // leftover result faces against input faces by measure. This is not a nicety; without it
    // ordinary boolean results come out partly anonymous.
    // The matched input FACE travels with the name, because a claim has to say which parent it
    // came from -- that is what separates a split from two bodies the naming layer cannot tell
    // apart. See settleClaims.
    // Every input face that shares a metric, not just the first.
    //
    // This was an emplace into a map keyed by metric, which silently kept the FIRST face with a
    // given measurement and discarded the rest -- so when two input faces measured the same, a
    // leftover result face was matched to whichever of them happened to be visited first. The
    // ambiguity did not disappear; only the evidence of it did.
    std::map<FaceMetric, std::vector<std::pair<ElementName, TopoDS_Shape>>> inputByMetric;
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        if (inputs[i]->isNull()) continue;
        for (const auto& face : inputs[i]->subShapes(kernel::ShapeType::Face)) {
            if (const auto n = inputMaps[i]->nameOf(face)) {
                const TopoDS_Shape& inFace = kernel::occt(const_cast<kernel::Shape&>(face));
                inputByMetric[measureFace(inFace)].emplace_back(*n, inFace);
            }
        }
    }
    for (const auto& face : resultFaces) {
        const TopoDS_Shape& of = kernel::occt(const_cast<kernel::Shape&>(face));
        if (claimedFaces.Contains(of)) continue;
        const auto it = inputByMetric.find(measureFace(of));
        // Exactly one candidate, or none. Several means this measurement does not identify an
        // input face, so there is no answer to give -- the result face is left unnamed and
        // `unnamed()` raises NamingLost. Picking one would be a guess wearing a fact's clothes.
        if (it != inputByMetric.end() && it->second.size() == 1) {
            claim(of, it->second.front().first, it->second.front().second);
        }
    }

    // Everything the reports and the fallback had to say is now in. Bound in one pass, so that a
    // name claimed by several faces can be split into siblings -- which is impossible to see from
    // inside a single claim.
    out = internal::resolveClaims(claims, impl_->featureSerial, impl_->opTag);

    // --- 3. genuinely new faces ---------------------------------------------------------
    // A cut's tool faces, for instance. They belong to this feature and are tagged by their
    // own measure so the tag is reproducible, not by iteration index.
    {
        std::vector<TopoDS_Shape> unclaimed;
        for (const auto& face : resultFaces) {
            if (out.nameOf(face)) continue;
            unclaimed.push_back(kernel::occt(const_cast<kernel::Shape&>(face)));
        }
        const auto newFaces = internal::canonicalOrder(std::move(unclaimed), internal::measureFace);
        for (std::size_t k = 0; k < newFaces.elements.size(); ++k) {
            if (newFaces.ambiguous[k] != 0) continue;
            NameStep s;
            s.featureSerial = impl_->featureSerial;
            s.opTag = impl_->opTag;
            s.provenance = Provenance::Generated;
            s.discriminator = static_cast<std::uint32_t>(k + 1);
            out.bind(kernel::wrap(newFaces.elements[k]), ElementName({s}));
        }
    }

    // --- 4. edges and vertices by boundary ----------------------------------------------
    if (auto r = impl_->deriveBoundaries(result, out); !r) return r.error();

    if (const auto missing = out.unnamed(result); !missing.empty()) {
        return kernel::Error{kernel::ErrorCode::NamingLost,
                             "Some geometry could not be identified after this operation.",
                             std::to_string(missing.size()) + " unnamed sub-elements"};
    }
    // The other half of the same guarantee. An element with NO name is caught above; two elements
    // with the SAME name are caught here, because `bind` keeps the last and the earlier one becomes
    // unreachable — a reference that silently means one arbitrary element of two is worse than one
    // that fails, and ADR 0005 exists so this is detectable at the moment it happens.
    if (const auto ambiguous = out.collisions(); !ambiguous.empty()) {
        return kernel::Error{kernel::ErrorCode::NamingLost,
                             "Two pieces of this shape ended up with the same identity, so a "
                             "reference to one of them would be ambiguous.",
                             std::to_string(ambiguous.size()) + " colliding names, first: "
                                 + ambiguous.front().toString()};
    }
    return out;
}

kernel::Result<ElementMap> NamingContext::nameCopy(const kernel::Operation& op,
                                                   const kernel::Shape& source,
                                                   const ElementMap& sourceMap,
                                                   NamingContext::Instance instance) {
    // Propagate first, which puts the SOURCE's names onto the copy's elements. That is the wrong
    // answer on its own -- it is what makes a copy indistinguishable from its source -- but it is
    // the correct correspondence, and correspondence is what the stamping below needs.
    auto moved = propagate(op, {&source}, {&sourceMap});
    if (!moved) return moved.error();

    NameStep step;
    step.featureSerial = impl_->featureSerial;
    step.opTag = impl_->opTag;
    step.provenance = Provenance::Generated;
    step.discriminator = instance.key();

    const auto stamp = [&step](const ElementName& name) {
        NameStep mine = step;
        mine.parents = {name.digest()};
        return name.derive(mine);
    };

    // Two passes, so that an ALIAS stays an alias. A name whose element is bound under a different
    // name is not a second element -- it is a merge's second reference, kept alive on purpose --
    // and binding both independently would give the copy two unrelated names for one face and lose
    // the relationship that says why.
    //
    // UNREACHABLE TODAY, and deliberately kept: `propagate` builds its output from each input
    // face's canonical name, so the map handed to the loop below has aliases only if the operation
    // itself merged faces, and no copy operation does -- a rigid transform maps one face to one
    // face. There is therefore no fixture that exercises this, and a test claiming to would be
    // asserting a property of an empty set. It is written the correct way round so that the first
    // copy operation that DOES merge does not silently flatten, which is the kind of thing that is
    // free now and archaeology later.
    ElementMap out;
    std::vector<std::pair<ElementName, ElementName>> aliases;   // canonical, alias
    for (const auto& name : moved.value().allNames()) {
        const auto shape = moved.value().resolve(name);
        if (!shape) continue;
        const auto canonical = moved.value().nameOf(*shape);
        if (canonical && *canonical == name) {
            out.bind(*shape, stamp(name));
        } else if (canonical) {
            aliases.emplace_back(*canonical, name);
        }
    }
    for (const auto& [canonical, alias] : aliases) out.alias(stamp(canonical), stamp(alias));

    if (const auto missing = out.unnamed(op.shape()); !missing.empty()) {
        return kernel::Error{kernel::ErrorCode::NamingLost,
                             "Some geometry could not be identified after copying this shape.",
                             std::to_string(missing.size()) + " unnamed sub-elements"};
    }
    // The same guard the other two entry points carry. It should be unreachable here -- one step
    // appended to distinct names leaves them distinct -- which is exactly why it is worth keeping:
    // if it ever fires, the assumption it rests on has stopped being true.
    if (const auto ambiguous = out.collisions(); !ambiguous.empty()) {
        return kernel::Error{kernel::ErrorCode::NamingLost,
                             "Two pieces of this copy ended up with the same identity.",
                             std::to_string(ambiguous.size()) + " colliding names, first: "
                                 + ambiguous.front().toString()};
    }
    return out;
}

}  // namespace cad::naming
