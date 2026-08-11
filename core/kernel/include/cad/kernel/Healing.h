#pragma once

#include "cad/kernel/Result.h"
#include "cad/kernel/Shape.h"

#include <string>
#include <vector>

namespace cad::kernel {

/// What healing had to do. Surfaced in the import report — see docs/FORMATS.md rule 1,
/// import never silently discards.
struct HealingReport {
    bool wasValid = false;
    bool isValidNow = false;
    std::vector<std::string> actions;
};

/// ShapeFix pipeline. Run on every import; foreign B-rep is routinely invalid.
Result<HealingReport> heal(Shape& inOut, double tolerance = 1e-7);

}  // namespace cad::kernel
