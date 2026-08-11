#pragma once

#include "cad/kernel/Result.h"
#include "cad/kernel/Shape.h"

#include <vector>

namespace cad::kernel {

/// Constant-radius fillet on the given edges.
///
/// `edges` must be sub-edges of `base` (resolved through the element map by the caller —
/// the kernel layer knows nothing about names). Passing an empty list is an error rather
/// than a no-op: it almost always means a naming lookup silently returned nothing.
Result<Operation> filletEdges(const Shape& base,
                              const std::vector<Shape>& edges,
                              double radius);

/// Symmetric chamfer on the given edges.
///
/// Same contract as filletEdges, including the empty-list-is-an-error rule. Naming-wise the
/// two are identical: both generate a new face FROM AN EDGE, which is why NamingContext
/// interrogates input edges and not just input faces.
Result<Operation> chamferEdges(const Shape& base,
                               const std::vector<Shape>& edges,
                               double distance);

}  // namespace cad::kernel
