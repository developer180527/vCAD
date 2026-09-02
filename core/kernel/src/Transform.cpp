#include "cad/kernel/Transform.h"
#include <cmath>
#include <gp_Ax1.hxx>
#include <BRepPrimAPI_MakeRevol.hxx>

#include "cad/kernel/Guard.h"
#include "cad/kernel/internal/Occt.h"

#include <BRepBuilderAPI_Transform.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <gp_Ax2.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>

#include <optional>

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

namespace {

/// Length of a direction vector, or nothing if it is too short to point anywhere.
///
/// Shared by rotate and mirror because both fail the same way without it: a degenerate direction
/// makes `gp_Dir` throw a Standard_ConstructionError from inside OCCT, which `guard` would turn
/// into a geometry error naming neither the argument nor the reason.
std::optional<double> directionLength(const double v[3]) {
    if (!isFinite(v[0]) || !isFinite(v[1]) || !isFinite(v[2])) return std::nullopt;
    const double length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    return length < 1e-12 ? std::nullopt : std::optional{length};
}

bool finitePoint(const double p[3]) {
    return isFinite(p[0]) && isFinite(p[1]) && isFinite(p[2]);
}

/// Applies a rigid `gp_Trsf` and keeps the algorithm, which is what carries the names across.
///
/// `BRepBuilderAPI_Transform` reports `Modified()` for every sub-shape it moves, so the naming
/// layer can follow a face through the move. Copying is deliberate: sharing the input's TShape
/// would make the "before" and "after" shapes `IsSame`, and the naming layer uses `IsSame` to
/// decide identity within a tree.
Result<Operation> applyTransform(const char* what, const Shape& s, const gp_Trsf& t) {
    return guard(what, [&] {
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

}  // namespace

Result<Operation> rotate(const Shape& s, const double origin[3], const double axis[3],
                         double angleRadians) {
    if (s.isNull()) {
        return Error{ErrorCode::InvalidInput, "Cannot transform an empty shape."};
    }
    if (!finitePoint(origin)) {
        return Error{ErrorCode::InvalidInput, "A rotation needs a finite centre."};
    }
    if (!isFinite(angleRadians)) {
        return Error{ErrorCode::InvalidInput, "A rotation needs a finite angle."};
    }
    const auto length = directionLength(axis);
    if (!length) {
        return Error{ErrorCode::InvalidInput, "A rotation needs an axis with a direction."};
    }

    gp_Trsf t;
    t.SetRotation(gp_Ax1(gp_Pnt(origin[0], origin[1], origin[2]),
                         gp_Dir(axis[0] / *length, axis[1] / *length, axis[2] / *length)),
                  angleRadians);
    return applyTransform("rotate", s, t);
}

Result<Operation> mirror(const Shape& s, const double origin[3], const double normal[3]) {
    if (s.isNull()) {
        return Error{ErrorCode::InvalidInput, "Cannot transform an empty shape."};
    }
    if (!finitePoint(origin)) {
        return Error{ErrorCode::InvalidInput, "A mirror needs a finite point on its plane."};
    }
    const auto length = directionLength(normal);
    if (!length) {
        return Error{ErrorCode::InvalidInput, "A mirror needs a plane with a normal direction."};
    }

    gp_Trsf t;
    // SetMirror(gp_Ax2), NOT SetMirror(gp_Ax1). The gp_Ax1 overload is a half-turn about the line
    // -- a rotation, determinant +1 -- and it is the standard way to write this operation and get
    // a part that is merely rotated. gp_Ax2's main direction is its plane's normal, so this
    // reflects in the plane the caller described.
    t.SetMirror(gp_Ax2(gp_Pnt(origin[0], origin[1], origin[2]),
                       gp_Dir(normal[0] / *length, normal[1] / *length, normal[2] / *length)));

    auto op = applyTransform("mirror", s, t);
    if (!op) return op;

    // The orientation check the header promises. A reflection flips handedness, and a solid whose
    // faces were not flipped with it encloses its own complement: `volume()` on it is negative or
    // zero, every boolean against it removes what it should have kept, and nothing about the shape
    // itself says so. Cheaper to find here than three operations downstream.
    const Shape reflected = op.value().shape();
    if (const auto valid = reflected.validate(); !valid) {
        return Error{ErrorCode::InvalidResult,
                     "Mirroring produced a shape that is not valid geometry.",
                     valid.error().detail};
    }
    if (s.volume() > 0.0 && reflected.volume() <= 0.0) {
        return Error{ErrorCode::InvalidResult,
                     "Mirroring turned the solid inside out.",
                     "The reflection reversed the faces' orientation without reversing the solid, "
                     "so it encloses everything except itself."};
    }
    return op;
}

Result<Operation> revolve(const Shape& profile, const double origin[3], const double axis[3],
                          double angleRadians) {
    const double length = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
    if (length < 1e-12) {
        return Error{ErrorCode::InvalidInput, "A revolve needs an axis with a direction."};
    }
    if (std::abs(angleRadians) < 1e-9) {
        return Error{ErrorCode::InvalidInput, "A revolve needs a non-zero angle."};
    }

    return guard("revolve the profile", [&] {
        const gp_Ax1 pivot(gp_Pnt(origin[0], origin[1], origin[2]),
                           gp_Dir(axis[0] / length, axis[1] / length, axis[2] / length));
        BRepPrimAPI_MakeRevol algo(occt(const_cast<Shape&>(profile)), pivot, angleRadians);
        algo.Build();
        Operation op;
        op.impl().algo = std::make_shared<BRepPrimAPI_MakeRevol>(std::move(algo));
        op.impl().result = op.impl().algo->Shape();
        return op;
    });
}

}  // namespace cad::kernel

namespace cad::kernel {

Result<Operation> extrude(const Shape& profile, double dx, double dy, double dz) {
    if (profile.isNull()) {
        return Error{ErrorCode::InvalidInput, "Cannot extrude an empty profile."};
    }
    if (!isFinite(dx) || !isFinite(dy) || !isFinite(dz)) {
        return Error{ErrorCode::InvalidInput, "A translation must be a finite distance."};
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
