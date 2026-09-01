#include "Measure.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepGProp.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <TopExp.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Vertex.hxx>
#include <gp_Pnt.hxx>

#include <algorithm>
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

BoundaryKey boundaryKeyOf(const TopoDS_Shape& s) {
    BoundaryKey key;
    key.midpoint = midpointOf(s);
    key.low = key.midpoint;
    key.high = key.midpoint;

    if (s.ShapeType() != TopAbs_EDGE) return key;   // a vertex is its own position and nothing more

    GProp_GProps props;
    BRepGProp::LinearProperties(s, props);
    key.extent = quantise(props.Mass());

    const TopoDS_Edge& edge = TopoDS::Edge(s);
    const TopoDS_Vertex first = TopExp::FirstVertex(edge);
    const TopoDS_Vertex last = TopExp::LastVertex(edge);
    if (!first.IsNull() && !last.IsNull()) {
        const gp_Pnt a = BRep_Tool::Pnt(first);
        const gp_Pnt b = BRep_Tool::Pnt(last);
        Point3 pa{quantise(a.X()), quantise(a.Y()), quantise(a.Z())};
        Point3 pb{quantise(b.X()), quantise(b.Y()), quantise(b.Z())};
        // SORTED. First and last depend on the edge's orientation, and the same edge reversed is
        // the same edge -- a key that swapped with orientation would reorder siblings for a reason
        // that has nothing to do with where they are.
        if (pb < pa) std::swap(pa, pb);
        key.low = pa;
        key.high = pb;
    }

    if (BRep_Tool::Degenerated(edge)) return key;
    const BRepAdaptor_Curve curve(edge);
    key.curve = static_cast<int>(curve.GetType());
    return key;
}

}  // namespace cad::naming::internal
