#include "cad/kernel/Fillet.h"

#include "cad/kernel/Guard.h"
#include "cad/kernel/internal/Occt.h"

#include <BRepFilletAPI_MakeChamfer.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>

namespace cad::kernel {

Result<Operation> chamferEdges(const Shape& base,
                               const std::vector<Shape>& edges,
                               double distance) {
    if (base.isNull()) {
        return Error{ErrorCode::InvalidInput, "Chamfer base shape is empty."};
    }
    if (edges.empty()) {
        // Same reasoning as filletEdges: an empty list means a naming lookup came back
        // empty, and quietly returning the unchamfered shape would hide it.
        return Error{ErrorCode::NamingLost,
                     "No edges to chamfer — the referenced edge could not be found.",
                     "chamferEdges called with an empty edge list"};
    }
    if (distance <= 0.0) {
        return Error{ErrorCode::InvalidInput, "Chamfer distance must be positive."};
    }

    return guard("chamferEdges", [&] {
        const TopoDS_Shape& baseShape = occt(const_cast<Shape&>(base));
        auto algo = std::make_shared<BRepFilletAPI_MakeChamfer>(baseShape);

        // Add(Dis, E) is the SYMMETRIC chamfer — equal setback on both faces, and no
        // reference face required. The four-argument overload takes two distances and a
        // face identifying which side Dis1 is measured from; that is asymmetric chamfer,
        // which will need its own entry point (and its own naming test) when we add it.
        //
        // Guard membership ourselves: OCCT's Add() silently does nothing for an edge that
        // is not part of the base shape, and a silent no-op here would surface much later
        // as a mysteriously missing chamfer.
        TopTools_IndexedMapOfShape baseEdges;
        TopExp::MapShapes(baseShape, TopAbs_EDGE, baseEdges);

        for (const auto& e : edges) {
            if (e.isNull() || e.type() != ShapeType::Edge) {
                throw std::runtime_error("chamfer input is not an edge");
            }
            const TopoDS_Shape& oe = occt(const_cast<Shape&>(e));
            if (!baseEdges.Contains(oe)) {
                throw std::runtime_error("chamfer edge does not belong to the base shape");
            }
            algo->Add(distance, TopoDS::Edge(oe));
        }

        algo->Build();
        if (!algo->IsDone()) {
            throw std::runtime_error("BRepFilletAPI_MakeChamfer::IsDone() == false");
        }
        Operation op;
        op.impl().algo = algo;
        op.impl().result = algo->Shape();
        op.impl().inputs = {baseShape};
        return op;
    });
}

}  // namespace cad::kernel
