#pragma once

#include "cad/kernel/Result.h"
#include "cad/kernel/Shape.h"

namespace cad::kernel {

Result<Operation> booleanCut(const Shape& base, const Shape& tool);
Result<Operation> booleanFuse(const Shape& a, const Shape& b);
Result<Operation> booleanCommon(const Shape& a, const Shape& b);

/// ShapeUpgrade_UnifySameDomain. Merges coplanar faces and collinear edges.
/// This is the operation that exercises Provenance::Merged in the naming layer.
Result<Operation> unifySameDomain(const Shape&);

}  // namespace cad::kernel
