#include "Measure.h"

#include <BRepGProp.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <TopoDS.hxx>
#include <gp_Pnt.hxx>

#include <cmath>

namespace cad::naming::internal {

std::int64_t quantise(double v) noexcept {
    return static_cast<std::int64_t>(std::llround(v * 1e7));
}

FaceMetric measureFace(const TopoDS_Shape& f) {
    GProp_GProps p;
    BRepGProp::SurfaceProperties(f, p);
    const gp_Pnt c = p.CentreOfMass();
    return {quantise(p.Mass()), quantise(c.X()), quantise(c.Y()), quantise(c.Z())};
}

Point3 midpointOf(const TopoDS_Shape& s) {
    // A vertex has no mass, so GProp would report the origin for every one of them and the
    // sibling ordering would collapse. Read the point directly.
    if (s.ShapeType() == TopAbs_VERTEX) {
        const gp_Pnt c = BRep_Tool::Pnt(TopoDS::Vertex(s));
        return {quantise(c.X()), quantise(c.Y()), quantise(c.Z())};
    }
    GProp_GProps p;
    if (s.ShapeType() == TopAbs_EDGE) {
        BRepGProp::LinearProperties(s, p);
    } else {
        BRepGProp::SurfaceProperties(s, p);
    }
    const gp_Pnt c = p.CentreOfMass();
    return {quantise(c.X()), quantise(c.Y()), quantise(c.Z())};
}

}  // namespace cad::naming::internal
