#pragma once
//
// INTERNAL. The only place OCCT types are allowed to meet our types.
//
// Includable ONLY from .cpp files inside core/kernel and core/naming. Never from a public
// header, never from render/, app/, or any shell. tools/check_layering.py does not police
// this one (it cannot tell a .cpp from a .h consumer); code review does.
//
// If you find yourself wanting this in a public header, you want a new opaque handle type
// instead.

#include "cad/kernel/Shape.h"

#include <BRepBuilderAPI_MakeShape.hxx>
#include <BRepTools_History.hxx>
#include <TopoDS_Shape.hxx>

#include <memory>

namespace cad::kernel {

struct Shape::Impl {
    TopoDS_Shape shape;
};

/// Type-erased owner of a live BRepBuilderAPI_MakeShape. The naming layer needs
/// Generated()/Modified()/IsDeleted() from the concrete algorithm, and those reports die
/// with the algorithm object — hence the ownership.
struct Operation::Impl {
    std::shared_ptr<BRepBuilderAPI_MakeShape> algo;  ///< null for primitives with no algo

    /// Some important algorithms are NOT BRepBuilderAPI_MakeShape — ShapeUpgrade_UnifySameDomain
    /// is the one that matters for merges. They expose a BRepTools_History instead, which
    /// carries the same Modified/Generated/Removed information. Naming consults whichever of
    /// the two is present.
    Handle(BRepTools_History) history;

    TopoDS_Shape result;

    /// Inputs the algorithm consumed, in the order the caller supplied them. Naming walks
    /// these to ask Modified()/Generated() about each input sub-element.
    std::vector<TopoDS_Shape> inputs;
};

inline const TopoDS_Shape& occt(const Shape& s) { return s.impl().shape; }
inline TopoDS_Shape& occt(Shape& s) { return s.impl().shape; }

Shape wrap(const TopoDS_Shape&);

}  // namespace cad::kernel
