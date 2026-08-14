#include "cad/kernel/Primitives.h"

#include "cad/kernel/Guard.h"
#include "cad/kernel/internal/Occt.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>

namespace cad::kernel {

Result<BoxResult> makeBox(double dx, double dy, double dz) {
    if (!isPositiveFinite(dx) || !isPositiveFinite(dy) || !isPositiveFinite(dz)) {
        return Error{ErrorCode::InvalidInput,
                     "Box dimensions must all be positive numbers."};
    }

    auto built = guard("makeBox", [&] {
        auto algo = std::make_shared<BRepPrimAPI_MakeBox>(dx, dy, dz);
        algo->Build();
        if (!algo->IsDone()) {
            throw std::runtime_error("BRepPrimAPI_MakeBox::IsDone() == false");
        }
        return algo;
    });
    if (!built) return built.error();

    auto algo = built.value();

    BoxResult out;
    out.op.impl().algo = algo;
    out.op.impl().result = algo->Shape();

    // The whole point of using the constructor's face accessors rather than an explorer:
    // these tags are positional and survive any dimension change. An explorer index does
    // not, and building on one is how naming silently breaks later.
    out.taggedFaces.resize(BoxFaceCount);
    auto tag = guard("makeBox/faces", [&] {
        out.taggedFaces[ZMin] = wrap(algo->BottomFace());
        out.taggedFaces[ZMax] = wrap(algo->TopFace());
        out.taggedFaces[XMax] = wrap(algo->FrontFace());   // yes, FrontFace() is x = dx
        out.taggedFaces[XMin] = wrap(algo->BackFace());
        out.taggedFaces[YMin] = wrap(algo->LeftFace());    // and LeftFace() is y = 0
        out.taggedFaces[YMax] = wrap(algo->RightFace());
    });
    if (!tag) return tag.error();

    return out;
}

Result<Operation> makeCylinder(double radius, double height) {
    if (!isPositiveFinite(radius) || !isPositiveFinite(height)) {
        return Error{ErrorCode::InvalidInput,
                     "Cylinder radius and height must be positive numbers."};
    }
    return guard("makeCylinder", [&] {
        auto algo = std::make_shared<BRepPrimAPI_MakeCylinder>(radius, height);
        algo->Build();
        if (!algo->IsDone()) {
            throw std::runtime_error("BRepPrimAPI_MakeCylinder::IsDone() == false");
        }
        Operation op;
        op.impl().algo = algo;
        op.impl().result = algo->Shape();
        return op;
    });
}

}  // namespace cad::kernel
