#include "cad/kernel/Primitives.h"

#include "cad/kernel/Guard.h"
#include "cad/kernel/internal/Occt.h"

#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
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

Result<Shape> makePlane(int plane, double size) {
    if (!(size > 0.0)) {
        return Error{ErrorCode::InvalidInput, "A plane needs a positive size."};
    }
    return guard("build the datum plane", [&] {
        const double h = size * 0.5;
        // Corners walked in a consistent order, so the face's normal comes out along the plane's
        // own axis rather than depending on which corner happened to be listed first.
        gp_Pnt a;
        gp_Pnt b;
        gp_Pnt c;
        gp_Pnt d;
        if (plane == 1) {          // XZ
            a = gp_Pnt(-h, 0.0, -h); b = gp_Pnt(h, 0.0, -h);
            c = gp_Pnt(h, 0.0, h);   d = gp_Pnt(-h, 0.0, h);
        } else if (plane == 2) {   // YZ
            a = gp_Pnt(0.0, -h, -h); b = gp_Pnt(0.0, h, -h);
            c = gp_Pnt(0.0, h, h);   d = gp_Pnt(0.0, -h, h);
        } else {                   // XY
            a = gp_Pnt(-h, -h, 0.0); b = gp_Pnt(h, -h, 0.0);
            c = gp_Pnt(h, h, 0.0);   d = gp_Pnt(-h, h, 0.0);
        }
        BRepBuilderAPI_MakePolygon polygon(a, b, c, d, /*Close*/ true);
        return wrap(BRepBuilderAPI_MakeFace(polygon.Wire(), /*OnlyPlane*/ true).Face());
    });
}

}  // namespace cad::kernel
