#include "cad/kernel/internal/Occt.h"

#include <TopTools_MapOfShape.hxx>

#include "cad/kernel/Guard.h"

#include <BRepAdaptor_Curve.hxx>
#include <gp_Lin.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <TopoDS.hxx>
#include <gp_Ax3.hxx>
#include <gp_Pln.hxx>
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

/// Why these two report failure as NaN rather than as zero.
///
/// Both return a plain value, not a `Result` -- they are called in comparisons and in log lines all
/// over the codebase, and threading a Result through every one of those would cost more than it
/// bought. But the OCCT calls beneath them can throw, and nothing above catches: no shell installs
/// a top-level handler, so an exception here ends the process rather than the operation.
///
/// So they are guarded, and a failure has to be represented in the return value. Zero is the wrong
/// choice and it is the tempting one: zero volume is what a FACE legitimately measures, and zero
/// area is what a degenerate edge measures, so a zero would be indistinguishable from a real answer
/// and would quietly pass `volume() > 0` style checks as a plain "no".
///
/// NaN cannot be mistaken for a measurement, and every comparison against it is false -- so a
/// caller asking `volume() > 0.0` gets "no" from a failure exactly as it does from an empty shape,
/// while a caller comparing two measurements never sees a failed one match anything, including
/// itself. That is the honest reading of "we could not measure this".
namespace {
constexpr double kUnmeasurable = std::numeric_limits<double>::quiet_NaN();
}  // namespace

Shape::Measure Shape::measure() const {
    if (isNull()) return {};
    const auto measured = guard("measure a shape", [&] {
        if (impl_->shape.ShapeType() == TopAbs_VERTEX) {
            const gp_Pnt p = BRep_Tool::Pnt(TopoDS::Vertex(impl_->shape));
            return Measure{0.0, p.X(), p.Y(), p.Z()};
        }
        GProp_GProps props;
        if (impl_->shape.ShapeType() == TopAbs_EDGE) {
            BRepGProp::LinearProperties(impl_->shape, props);
        } else {
            BRepGProp::SurfaceProperties(impl_->shape, props);
        }
        const gp_Pnt c = props.CentreOfMass();
        return Measure{props.Mass(), c.X(), c.Y(), c.Z()};
    });
    if (!measured) {
        return Measure{kUnmeasurable, kUnmeasurable, kUnmeasurable, kUnmeasurable};
    }
    return measured.value();
}

double Shape::volume() const {
    if (isNull()) return 0.0;
    const auto measured = guard("measure a shape's volume", [&] {
        GProp_GProps props;
        BRepGProp::VolumeProperties(impl_->shape, props);
        return props.Mass();
    });
    return measured ? measured.value() : kUnmeasurable;
}

Operation::Operation() : impl_(std::make_unique<Impl>()) {}
Operation::~Operation() = default;
Operation::Operation(Operation&&) noexcept = default;
Operation& Operation::operator=(Operation&&) noexcept = default;

Shape Operation::shape() const { return wrap(impl_->result); }

std::vector<Shape> subShapes(const Shape& shape, SubShape kind) {
    std::vector<Shape> out;
    if (shape.isNull()) return out;

    TopAbs_ShapeEnum wanted = TopAbs_FACE;
    switch (kind) {
        case SubShape::Face:   wanted = TopAbs_FACE;   break;
        case SubShape::Edge:   wanted = TopAbs_EDGE;   break;
        case SubShape::Vertex: wanted = TopAbs_VERTEX; break;
    }

    TopTools_MapOfShape seen;
    for (TopExp_Explorer it(occt(shape), wanted); it.More(); it.Next()) {
        if (seen.Add(it.Current())) out.push_back(wrap(it.Current()));
    }
    return out;
}

}  // namespace cad::kernel

namespace cad::kernel {

Result<LineFrame> lineOf(const Shape& edge) {
    auto guarded = guard("measure the edge's line", [&]() -> Result<LineFrame> {
        const TopoDS_Shape& shape = occt(const_cast<Shape&>(edge));
        if (shape.IsNull() || shape.ShapeType() != TopAbs_EDGE) {
            return Error{ErrorCode::InvalidInput, "That is not an edge."};
        }
        BRepAdaptor_Curve curve(TopoDS::Edge(shape));
        if (curve.GetType() != GeomAbs_Line) {
            return Error{ErrorCode::InvalidInput,
                         "That edge is not straight, so it cannot be an axis."};
        }
        const gp_Lin line = curve.Line();
        LineFrame frame;
        frame.origin[0] = line.Location().X();
        frame.origin[1] = line.Location().Y();
        frame.origin[2] = line.Location().Z();
        frame.direction[0] = line.Direction().X();
        frame.direction[1] = line.Direction().Y();
        frame.direction[2] = line.Direction().Z();
        return frame;
    });
    if (!guarded) return guarded.error();
    return guarded.value();
}

Result<PlaneFrame> planeOf(const Shape& face) {
    if (face.isNull()) {
        return Error{ErrorCode::InvalidInput, "There is no face to measure."};
    }
    // guard() wraps whatever the lambda returns, so a lambda returning Result<PlaneFrame> yields
    // Result<Result<PlaneFrame>>. Flattened explicitly, because the failures below are OURS -- not
    // flat, not a face -- and deserve their own messages rather than an OCCT exception translated
    // into something generic.
    auto guarded = guard("measure a face's plane", [&]() -> Result<PlaneFrame> {
        const TopoDS_Shape& shape = occt(face);
        if (shape.ShapeType() != TopAbs_FACE) {
            return Error{ErrorCode::InvalidInput,
                         "A sketch can only be placed on a face.",
                         "shape is not a TopoDS_FACE"};
        }

        BRepAdaptor_Surface surface(TopoDS::Face(shape));
        if (surface.GetType() != GeomAbs_Plane) {
            // Refused rather than approximated. A cylinder or a spline has no single plane, and
            // choosing one would place the sketch somewhere the user did not pick.
            return Error{ErrorCode::Unsupported,
                         "This face is not flat, so a sketch cannot be placed on it."};
        }

        const gp_Pln plane = surface.Plane();
        const gp_Ax3 axis = plane.Position();
        const gp_Pnt origin = axis.Location();
        const gp_Dir u = axis.XDirection();
        const gp_Dir v = axis.YDirection();
        const gp_Dir n = axis.Direction();

        PlaneFrame frame;
        frame.origin[0] = origin.X();
        frame.origin[1] = origin.Y();
        frame.origin[2] = origin.Z();
        frame.u[0] = u.X();  frame.u[1] = u.Y();  frame.u[2] = u.Z();
        frame.v[0] = v.X();  frame.v[1] = v.Y();  frame.v[2] = v.Z();
        frame.normal[0] = n.X();  frame.normal[1] = n.Y();  frame.normal[2] = n.Z();
        return frame;
    });
    if (!guarded) return guarded.error();
    return guarded.value();
}

}  // namespace cad::kernel
