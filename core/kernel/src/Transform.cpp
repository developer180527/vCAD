#include "cad/kernel/Transform.h"

#include "cad/kernel/Guard.h"
#include "cad/kernel/internal/Occt.h"

#include <BRepBuilderAPI_Transform.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
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

namespace cad::kernel {

Result<Operation> extrude(const Shape& profile, double dx, double dy, double dz) {
    if (profile.isNull()) {
        return Error{ErrorCode::InvalidInput, "Cannot extrude an empty profile."};
    }
    if (dx == 0.0 && dy == 0.0 && dz == 0.0) {
        // Caught here rather than inside OCCT, which builds a degenerate solid of zero volume and
        // reports success -- a "solid" that then fails every downstream operation for no stated
        // reason.
        return Error{ErrorCode::InvalidInput, "An extrusion needs a non-zero distance."};
    }
    return guard("extrude", [&] {
        auto algo = std::make_shared<BRepPrimAPI_MakePrism>(occt(const_cast<Shape&>(profile)),
                                                           gp_Vec(dx, dy, dz), /*Copy*/ false,
                                                           /*Canonize*/ true);
        algo->Build();
        if (!algo->IsDone()) {
            throw std::runtime_error("BRepPrimAPI_MakePrism::IsDone() == false");
        }
        Operation op;
        op.impl().algo = algo;
        op.impl().result = algo->Shape();
        op.impl().inputs = {occt(const_cast<Shape&>(profile))};
        return op;
    });
}

}  // namespace cad::kernel
