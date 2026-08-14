#include "cad/kernel/Booleans.h"

#include "cad/kernel/Guard.h"
#include "cad/kernel/internal/Occt.h"

#include <TopExp_Explorer.hxx>
#include <TopAbs_ShapeEnum.hxx>

#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>

namespace cad::kernel {
namespace {

template <class Algo>
Result<Operation> runBoolean(const char* what, const Shape& a, const Shape& b) {
    if (a.isNull() || b.isNull()) {
        return Error{ErrorCode::InvalidInput, "Boolean operands must not be empty."};
    }
    auto r = guard(what, [&] {
        auto algo = std::make_shared<Algo>(TopoDS_Shape(occt(a)),
                                           TopoDS_Shape(occt(b)));
        algo->Build();
        if (!algo->IsDone()) {
            throw std::runtime_error("boolean algorithm reported not-done");
        }
        const TopoDS_Shape result = algo->Shape();

        // An EMPTY result is not a success.
        //
        // A cut whose tool exactly covers its base removes everything, and OCCT reports that as
        // done: IsDone() is true, Shape() is a valid compound, and BRepCheck_Analyzer is
        // perfectly happy with it — because a compound containing nothing is a legal shape. It
        // just has no solids, no faces and no volume.
        //
        // Left alone, that flows downstream as a feature marked Clean whose shape is nothing:
        // every later boolean quietly does nothing, the mesh has no triangles, and the part
        // disappears from the viewport with no error anywhere. Found by the geometry torture
        // suite, which cut a 40mm box with an identical 40mm box.
        //
        // Checked on FACE rather than SOLID: a boolean can legitimately reduce a solid to a
        // shell or a face in degenerate-but-meaningful cases, and rejecting those would be
        // stricter than the kernel needs to be. No faces at all is unambiguous.
        if (result.IsNull() || !TopExp_Explorer(result, TopAbs_FACE).More()) {
            throw std::runtime_error("the operation removed all geometry; the result is empty");
        }

        Operation op;
        op.impl().algo = algo;
        op.impl().result = result;
        op.impl().inputs = {TopoDS_Shape(occt(a)), TopoDS_Shape(occt(b))};
        return op;
    });
    if (!r) {
        // Booleans are the least robust part of OCCT. Give the caller a specific code so the
        // UI can say "this cut failed" rather than "something went wrong".
        return Error{ErrorCode::BooleanFailed, r.error().message, r.error().detail};
    }
    return r;
}

}  // namespace

Result<Operation> booleanCut(const Shape& base, const Shape& tool) {
    return runBoolean<BRepAlgoAPI_Cut>("booleanCut", base, tool);
}
Result<Operation> booleanFuse(const Shape& a, const Shape& b) {
    return runBoolean<BRepAlgoAPI_Fuse>("booleanFuse", a, b);
}
Result<Operation> booleanCommon(const Shape& a, const Shape& b) {
    return runBoolean<BRepAlgoAPI_Common>("booleanCommon", a, b);
}

Result<Operation> unifySameDomain(const Shape& s) {
    if (s.isNull()) {
        return Error{ErrorCode::InvalidInput, "Cannot unify an empty shape."};
    }
    // ShapeUpgrade_UnifySameDomain is NOT a BRepBuilderAPI_MakeShape, so it cannot go in
    // Operation::algo. It exposes History() instead — and that history is the ONLY way to
    // learn which input faces were merged into which output face. Geometric matching cannot
    // recover it, because a merged face has neither the area nor the centroid of either
    // parent. Dropping the history here would make every merge anonymous.
    return guard("unifySameDomain", [&] {
        ShapeUpgrade_UnifySameDomain unifier(TopoDS_Shape(occt(s)), true, true, false);
        unifier.Build();
        Operation op;
        op.impl().algo = nullptr;
        op.impl().history = unifier.History();
        op.impl().result = unifier.Shape();
        op.impl().inputs = {TopoDS_Shape(occt(s))};
        return op;
    });
}

}  // namespace cad::kernel
