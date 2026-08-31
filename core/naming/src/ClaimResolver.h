#pragma once

/// Turning CLAIMS into an element map.
///
/// # What a claim is
///
/// While naming an operation's result, `NamingContext::propagate` asks OCCT what happened and works
/// out, for each result face, which input element's name it should carry. Each of those answers is
/// a claim: *this result face, under this name, because of that input element*.
///
/// # Why this is a separate thing
///
/// Claims cannot be bound as they arrive. Several faces claiming one name mean two opposite things
/// -- a face that SPLIT into pieces, or two elements the naming layer cannot tell apart -- and
/// neither is visible from inside a single claim. You have to have them all before you can name any
/// of them. That is a self-contained decision over data, and it is where every naming bug found so
/// far has actually lived.
///
/// Separating it from `propagate` is not tidying. `propagate` can only be exercised by building
/// real geometry and running a real boolean, which means split-versus-merge, sibling ordering, tie
/// handling and alias canonicity were all reachable only through OCCT -- and a mutation that
/// deleted the sibling ordering entirely passed the whole suite, because nothing could get at it
/// directly. This half needs no operation, no reporting, and no algorithm: hand it shapes and
/// names and it answers.

#include "cad/naming/ElementMap.h"

#include <TopoDS_Shape.hxx>

#include <cstdint>
#include <vector>

namespace cad::naming::internal {

/// One result element claiming a name, and the input element it claimed it from.
struct Claim {
    TopoDS_Shape face;     ///< the RESULT element being claimed
    ElementName name;      ///< what it would be called
    TopoDS_Shape parent;   ///< the INPUT element the name came from
};

/// Binds every claim, discriminating the pieces of a split and aliasing the halves of a merge.
///
/// `featureSerial` and `opTag` stamp the extra derivation step a split piece gets; they identify
/// the operation doing the splitting.
///
/// Deliberately total: it never fails. A claim set it cannot name unambiguously produces a map with
/// colliding names, which `ElementMap::collisions` reports and the caller turns into `NamingLost`.
/// Splitting that judgement across two layers is how a check ends up applied in one path and not
/// another.
[[nodiscard]] ElementMap resolveClaims(const std::vector<Claim>& claims,
                                       std::uint32_t featureSerial, std::uint16_t opTag);

}  // namespace cad::naming::internal
