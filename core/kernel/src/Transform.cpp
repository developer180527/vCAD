#include "cad/kernel/Transform.h"

#include "cad/kernel/Guard.h"
#include "cad/kernel/internal/Occt.h"

#include <BRepBuilderAPI_Transform.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>

namespace cad::kernel {

Result<Operation> translate(const Shape& s, double dx, double dy, double dz) {
    if (s.isNull()) {
        return Error{ErrorCode::InvalidInput, "Cannot transform an empty shape."};
    }
    return guard("translate", [&] {
        gp_Trsf t;
        t.SetTranslation(gp_Vec(dx, dy, dz));
        auto algo = std::make_shared<BRepBuilderAPI_Transform>(
            occt(const_cast<Shape&>(s)), t, /*copy*/ true);
        algo->Build();
        if (!algo->IsDone()) {
            throw std::runtime_error("BRepBuilderAPI_Transform::IsDone() == false");
        }
        Operation op;
        op.impl().algo = algo;
        op.impl().result = algo->Shape();
        op.impl().inputs = {occt(const_cast<Shape&>(s))};
        return op;
    });
}

}  // namespace cad::kernel
