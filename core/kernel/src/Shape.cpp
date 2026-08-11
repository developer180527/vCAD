#include "cad/kernel/internal/Occt.h"

#include "cad/kernel/Guard.h"

#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <TopoDS.hxx>
#include <gp_Pnt.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopExp.hxx>

#include <cstdio>

namespace cad::kernel {
namespace {

TopAbs_ShapeEnum toOcct(ShapeType t) {
    switch (t) {
        case ShapeType::Compound:  return TopAbs_COMPOUND;
        case ShapeType::CompSolid: return TopAbs_COMPSOLID;
        case ShapeType::Solid:     return TopAbs_SOLID;
        case ShapeType::Shell:     return TopAbs_SHELL;
        case ShapeType::Face:      return TopAbs_FACE;
        case ShapeType::Wire:      return TopAbs_WIRE;
        case ShapeType::Edge:      return TopAbs_EDGE;
        case ShapeType::Vertex:    return TopAbs_VERTEX;
        case ShapeType::Unknown:   break;
    }
    return TopAbs_SHAPE;
}

ShapeType fromOcct(TopAbs_ShapeEnum t) {
    switch (t) {
        case TopAbs_COMPOUND:  return ShapeType::Compound;
        case TopAbs_COMPSOLID: return ShapeType::CompSolid;
        case TopAbs_SOLID:     return ShapeType::Solid;
        case TopAbs_SHELL:     return ShapeType::Shell;
        case TopAbs_FACE:      return ShapeType::Face;
        case TopAbs_WIRE:      return ShapeType::Wire;
        case TopAbs_EDGE:      return ShapeType::Edge;
        case TopAbs_VERTEX:    return ShapeType::Vertex;
        default:               return ShapeType::Unknown;
    }
}

}  // namespace

const char* toString(ShapeType t) noexcept {
    switch (t) {
        case ShapeType::Compound:  return "Compound";
        case ShapeType::CompSolid: return "CompSolid";
        case ShapeType::Solid:     return "Solid";
        case ShapeType::Shell:     return "Shell";
        case ShapeType::Face:      return "Face";
        case ShapeType::Wire:      return "Wire";
        case ShapeType::Edge:      return "Edge";
        case ShapeType::Vertex:    return "Vertex";
        case ShapeType::Unknown:   return "Unknown";
    }
    return "Unknown";
}

std::string ShapeHash::hex() const {
    char buf[65];
    std::snprintf(buf, sizeof(buf), "%016llx%016llx%016llx%016llx",
                  static_cast<unsigned long long>(lanes[0]),
                  static_cast<unsigned long long>(lanes[1]),
                  static_cast<unsigned long long>(lanes[2]),
                  static_cast<unsigned long long>(lanes[3]));
    return std::string(buf, 64);
}

Shape wrap(const TopoDS_Shape& s) {
    Shape out;
    out.impl().shape = s;
    return out;
}

Shape::Shape() : impl_(std::make_unique<Impl>()) {}
Shape::~Shape() = default;
Shape::Shape(const Shape& o) : impl_(std::make_unique<Impl>(*o.impl_)) {}
Shape::Shape(Shape&&) noexcept = default;
Shape& Shape::operator=(Shape&&) noexcept = default;

Shape& Shape::operator=(const Shape& o) {
    if (this != &o) {
        impl_ = std::make_unique<Impl>(*o.impl_);
    }
    return *this;
}

bool Shape::isNull() const noexcept { return impl_->shape.IsNull(); }

ShapeType Shape::type() const {
    if (isNull()) return ShapeType::Unknown;
    return fromOcct(impl_->shape.ShapeType());
}

bool Shape::isSame(const Shape& other) const {
    if (isNull() || other.isNull()) return false;
    return impl_->shape.IsSame(other.impl_->shape) == Standard_True;
}

std::vector<Shape> Shape::subShapes(ShapeType of) const {
    std::vector<Shape> out;
    if (isNull()) return out;
    // TopExp::MapShapes deduplicates and, crucially, visits in a deterministic order that
    // depends only on the topology tree — not on pointer values. Do not replace this with a
    // hash-map walk; that is exactly how cross-process non-determinism gets in.
    TopTools_IndexedMapOfShape map;
    TopExp::MapShapes(impl_->shape, toOcct(of), map);
    out.reserve(static_cast<std::size_t>(map.Extent()));
    for (int i = 1; i <= map.Extent(); ++i) {
        out.push_back(wrap(map(i)));
    }
    return out;
}

Result<void> Shape::validate() const {
    if (isNull()) {
        return Error{ErrorCode::InvalidInput, "Shape is empty."};
    }
    return guard("validate", [&] {
        BRepCheck_Analyzer analyzer(impl_->shape);
        if (!analyzer.IsValid()) {
            throw std::runtime_error("BRepCheck_Analyzer reported an invalid shape");
        }
    });
}

Shape::Measure Shape::measure() const {
    if (isNull()) return {};
    if (impl_->shape.ShapeType() == TopAbs_VERTEX) {
        const gp_Pnt p = BRep_Tool::Pnt(TopoDS::Vertex(impl_->shape));
        return {0.0, p.X(), p.Y(), p.Z()};
    }
    GProp_GProps props;
    if (impl_->shape.ShapeType() == TopAbs_EDGE) {
        BRepGProp::LinearProperties(impl_->shape, props);
    } else {
        BRepGProp::SurfaceProperties(impl_->shape, props);
    }
    const gp_Pnt c = props.CentreOfMass();
    return {props.Mass(), c.X(), c.Y(), c.Z()};
}

Operation::Operation() : impl_(std::make_unique<Impl>()) {}
Operation::~Operation() = default;
Operation::Operation(Operation&&) noexcept = default;
Operation& Operation::operator=(Operation&&) noexcept = default;

Shape Operation::shape() const { return wrap(impl_->result); }

}  // namespace cad::kernel
