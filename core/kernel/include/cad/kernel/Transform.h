#pragma once

#include "cad/kernel/Result.h"
#include "cad/kernel/Shape.h"

namespace cad::kernel {

/// Rigid translation. Returns an Operation rather than a Shape because
/// BRepBuilderAPI_Transform reports Modified(), which is how names survive the move.
Result<Operation> translate(const Shape&, double dx, double dy, double dz);

}  // namespace cad::kernel
