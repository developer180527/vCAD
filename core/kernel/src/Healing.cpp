#include "cad/kernel/Healing.h"

#include "cad/kernel/Guard.h"
#include "cad/kernel/internal/Occt.h"

#include <BRepCheck_Analyzer.hxx>
#include <ShapeFix_Shape.hxx>

namespace cad::kernel {

Result<HealingReport> heal(Shape& inOut, double tolerance) {
    if (inOut.isNull()) {
        return Error{ErrorCode::InvalidInput, "Cannot heal an empty shape."};
    }
    return guard("heal", [&] {
        HealingReport report;
        report.wasValid = BRepCheck_Analyzer(occt(inOut)).IsValid() == Standard_True;
        if (report.wasValid) {
            report.isValidNow = true;
            return report;
        }
        ShapeFix_Shape fixer(occt(inOut));
        fixer.SetPrecision(tolerance);
        fixer.Perform();
        occt(inOut) = fixer.Shape();
        report.actions.emplace_back("ShapeFix_Shape");
        report.isValidNow = BRepCheck_Analyzer(occt(inOut)).IsValid() == Standard_True;
        return report;
    });
}

}  // namespace cad::kernel
