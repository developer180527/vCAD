#include "cad/kernel/Booleans.h"

#include "cad/kernel/Guard.h"
#include "cad/kernel/internal/Occt.h"

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
        Operation op;
        op.impl().algo = algo;
        op.impl().result = algo->Shape();
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
