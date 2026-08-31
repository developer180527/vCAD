#include "ClaimResolver.h"

#include "Measure.h"
#include "cad/kernel/internal/Occt.h"

#include <algorithm>
#include <vector>

namespace cad::naming::internal {

ElementMap resolveClaims(const std::vector<Claim>& claims, std::uint32_t featureSerial,
                         std::uint16_t opTag) {
    ElementMap out;
    // Grouped by NAME AND PARENT, and the parent half is what keeps this honest.
    //
    // Several result faces sharing a name mean one of two completely different things:
    //
    //   * they came from the SAME input face, which split. They are pieces of one element and
    //     numbering them is right.
    //   * they came from DIFFERENT input faces that happen to carry the same name -- which is
    //     what an unstamped COPY produces. Those are two unrelated elements that the naming
    //     layer cannot tell apart, and numbering them by area order would assign a user's
    //     reference to one of two bodies by geometric accident. That must stay a collision so
    //     the guard refuses it.
    //
    // Grouping on the name alone conflates the two and silently "fixes" the second, which is
    // the more dangerous of them.
    struct Siblings {
        std::uint64_t name = 0;
        TopoDS_Shape parent;
        std::vector<TopoDS_Shape> faces;
    };
    std::vector<Siblings> groups;
    const auto groupFor = [&](const Claim& c) -> Siblings& {
        for (auto& g : groups) {
            if (g.name == c.name.digest() && g.parent.IsSame(c.parent)) return g;
        }
        groups.push_back(Siblings{c.name.digest(), c.parent, {}});
        return groups.back();
    };
    for (const auto& c : claims) {
        auto& group = groupFor(c);
        const bool seen = std::any_of(group.faces.begin(), group.faces.end(),
                                      [&](const TopoDS_Shape& f) { return f.IsSame(c.face); });
        if (!seen) group.faces.push_back(c.face);
    }

    // Canonical order within each split, so the pieces are numbered the same way on every
    // rebuild and on every machine. Measure order -- area then centroid, quantised -- is what
    // the rest of this file already uses for exactly this purpose.
    //
    // It inherits that scheme's known weakness: two pieces of identical area and centroid
    // cannot be separated, and a change that swaps two pieces' positions swaps their names.
    // That is the same fragility ADR 0005 records for split edges, and no stronger invariant
    // is available without a change to the naming scheme itself.
    // Canonical order, or nothing. See canonicalOrder in Measure.h for why a tie clears the group
    // rather than picking a winner: an undiscriminated group leaves its pieces sharing one name,
    // which `collisions()` reports and the caller turns into NamingLost.
    for (auto& group : groups) {
        if (group.faces.size() < 2) continue;
        auto ordered = canonicalOrder(std::move(group.faces),
                                      [](const TopoDS_Shape& f) { return measureFace(f); });
        // Tied pieces are dropped from the group, so they fall back to the parent's own name and
        // collide -- which `collisions()` reports. The rest keep their positions and their numbers.
        std::vector<TopoDS_Shape> keep;
        for (std::size_t i = 0; i < ordered.elements.size(); ++i) {
            if (ordered.ambiguous[i] == 0) keep.push_back(ordered.elements[i]);
        }
        group.faces = std::move(keep);
    }

    // The final name for a claim: unchanged when the name has one claimant, and derived with a
    // sibling index when it has several. Unchanged is the important half -- a face that did not
    // split keeps exactly the name it had before this fix existed, so no reference in any saved
    // document moves.
    const auto finalName = [&](const Claim& c) {
        const ElementName& name = c.name;
        const auto& faces = groupFor(c).faces;
        if (faces.size() < 2) return name;
        const auto at = std::find_if(faces.begin(), faces.end(),
                                     [&](const TopoDS_Shape& f) { return f.IsSame(c.face); });
        NameStep step;
        step.featureSerial = featureSerial;
        step.opTag = opTag;
        // Modified, not Generated: a split piece IS the parent face, in part. Calling it
        // Generated would say the operation created something new, and "the top face" would
        // stop meaning anything about the pieces it became.
        step.provenance = Provenance::Modified;
        step.discriminator = static_cast<std::uint32_t>(at - faces.begin()) + 1;
        step.parents = {name.digest()};
        return name.derive(step);
    };

    // Result faces claimed so far, keyed by the FACE ITSELF -- by topological identity, not by
    // its measurements.
    //
    // This used to be keyed by FaceMetric, which is a quantised (area, centroid) tuple and is
    // therefore not an identity: two genuinely different result faces sharing one metric were
    // treated as the same face and sent down the merge path, aliasing two unrelated names
    // together. A measurement is a heuristic for MATCHING; it is never an identity, and the
    // comment here claimed identity while the container provided a measurement.
    std::vector<std::pair<TopoDS_Shape, ElementName>> claimed;
    const auto findClaim = [&claimed](const TopoDS_Shape& face) {
        return std::find_if(claimed.begin(), claimed.end(), [&](const auto& e) {
            return e.first.IsSame(face);
        });
    };
    for (const auto& c : claims) {
        const TopoDS_Shape& face = c.face;
        ElementName resolved = finalName(c);
        const auto it = findClaim(face);
        if (it == claimed.end()) {
            claimed.emplace_back(face, resolved);
            out.bind(kernel::wrap(face), std::move(resolved));
            continue;
        }
        if (it->second == resolved) continue;   // the same claim twice; nothing to alias
        // Two input faces landed on one result face: a merge (two coplanar faces unified,
        // say). BOTH names must resolve to the face — a user reference to either one has to
        // survive — so bind the newcomer as well, and record which name is canonical.
        //
        // Binding is what makes resolution work; the alias entry only records canonicity.
        // Recording the alias without binding would leave the smaller name unresolvable,
        // which is the subtle version of exactly the bug this layer exists to prevent.
        out.bind(kernel::wrap(face), resolved);
        if (resolved < it->second) {
            out.alias(resolved, it->second);
            it->second = resolved;
        } else {
            out.alias(it->second, resolved);
        }
    }
    return out;
}

}  // namespace cad::naming::internal
