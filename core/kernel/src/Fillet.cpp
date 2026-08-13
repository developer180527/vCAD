#include "cad/kernel/Fillet.h"

#include "cad/kernel/Guard.h"
#include "cad/kernel/internal/Occt.h"

#include <BRepFilletAPI_MakeFillet.hxx>
#include <TopoDS.hxx>

namespace cad::kernel {

Result<Operation> filletEdges(const Shape& base,
                              const std::vector<Shape>& edges,
                              double radius) {
    if (base.isNull()) {
        return Error{ErrorCode::InvalidInput, "Fillet base shape is empty."};
    }
    if (edges.empty()) {
        // Deliberately an error, not a no-op. An empty edge list almost always means an
        // element-map lookup returned nothing, and silently producing the unfilleted shape
        // would hide exactly the bug this milestone exists to prevent.
        return Error{ErrorCode::NamingLost,
                     "No edges to fillet — the referenced edge could not be found.",
                     "filletEdges called with an empty edge list"};
    }
    if (radius <= 0.0) {
        return Error{ErrorCode::InvalidInput, "Fillet radius must be positive."};
    }

    return guard("filletEdges", [&] {
        auto algo = std::make_shared<BRepFilletAPI_MakeFillet>(TopoDS_Shape(occt(base)));
        for (const auto& e : edges) {
            if (e.isNull() || e.type() != ShapeType::Edge) {
                throw std::runtime_error("fillet input is not an edge");
            }
            algo->Add(radius, TopoDS::Edge(TopoDS_Shape(occt(e))));
        }
        algo->Build();
        if (!algo->IsDone()) {
            throw std::runtime_error("BRepFilletAPI_MakeFillet::IsDone() == false");
        }
        Operation op;
        op.impl().algo = algo;
        op.impl().result = algo->Shape();
        op.impl().inputs = {TopoDS_Shape(occt(base))};
        return op;
    });
}

}  // namespace cad::kernel
