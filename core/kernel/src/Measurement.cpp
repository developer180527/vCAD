#include "cad/kernel/Measurement.h"

#include "cad/kernel/Guard.h"
#include "cad/kernel/internal/Occt.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepGProp.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <TopoDS.hxx>
#include <gp_Pnt.hxx>

namespace cad::kernel {
namespace {

/// The radius an element has, if it has one.
///
/// Asked of the ADAPTOR rather than of the geometry handle, because a trimmed arc, a full circle
/// and a circle on a face all answer the same way through it, and the alternative is a chain of
/// DownCasts that gets one of those cases wrong.
void fillRadius(const TopoDS_Shape& raw, Measurement& out) {
    if (raw.ShapeType() == TopAbs_EDGE) {
        const TopoDS_Edge& edge = TopoDS::Edge(raw);
        if (BRep_Tool::Degenerated(edge)) return;
        const BRepAdaptor_Curve curve(edge);
        if (curve.GetType() == GeomAbs_Circle) {
            out.radius = curve.Circle().Radius();
            out.hasRadius = true;
        }
        return;
    }
    if (raw.ShapeType() != TopAbs_FACE) return;
    const BRepAdaptor_Surface surface(TopoDS::Face(raw));
    if (surface.GetType() == GeomAbs_Cylinder) {
        out.radius = surface.Cylinder().Radius();
        out.hasRadius = true;
    } else if (surface.GetType() == GeomAbs_Sphere) {
        out.radius = surface.Sphere().Radius();
        out.hasRadius = true;
    }
}

}  // namespace

Result<Measurement> measure(const Shape& shape) {
    if (shape.isNull()) {
        return Error{ErrorCode::InvalidInput, "There is nothing here to measure."};
    }
    return guard("measure a shape", [&]() -> Measurement {
        const TopoDS_Shape& raw = occt(const_cast<Shape&>(shape));

        Measurement out;
        out.type = shape.type();

        if (raw.ShapeType() == TopAbs_VERTEX) {
            const gp_Pnt p = BRep_Tool::Pnt(TopoDS::Vertex(raw));
            out.centre[0] = p.X();
            out.centre[1] = p.Y();
            out.centre[2] = p.Z();
            return out;
        }

        // Each property in its own pass, because OCCT computes them separately and a shape can
        // legitimately have length and no volume. Asking for all three of a face and reporting
        // whichever came back non-zero would make an open shell look like a solid.
        GProp_GProps linear;
        BRepGProp::LinearProperties(raw, linear);
        out.length = linear.Mass();

        GProp_GProps surface;
        BRepGProp::SurfaceProperties(raw, surface);
        out.area = surface.Mass();

        // Volume only for something that encloses one, because a face bounds nothing and a
        // "volume" for it would be an artefact of where the origin happens to be.
        //
        // Defensive rather than load-bearing, and measured as such: removing this condition does
        // not change any answer, because BRepGProp already returns zero for a lone face here. It
        // states the intent -- and it stops the day someone measures a face far from the origin,
        // or OCCT changes its mind -- but no test can currently tell the two versions apart, and
        // claiming otherwise would be the flattering version of this comment.
        GProp_GProps volume;
        // SHELL included: a closed shell encloses a volume and reporting none for it would be a
        // missing answer rather than a refused one. An open shell encloses nothing and OCCT says so
        // by measuring near zero, which is the honest result for it.
        const bool encloses = raw.ShapeType() == TopAbs_SOLID
                              || raw.ShapeType() == TopAbs_COMPSOLID
                              || raw.ShapeType() == TopAbs_SHELL
                              || raw.ShapeType() == TopAbs_COMPOUND;
        if (encloses) {
            BRepGProp::VolumeProperties(raw, volume);
            out.volume = volume.Mass();
        }

        // The centre that MEANS something for this kind: an edge's midpoint comes from its length,
        // a face's centroid from its area, a solid's from its volume. Taking the highest dimension
        // that actually has mass is what makes "centre" the same idea at every level.
        const GProp_GProps& centre =
            out.volume != 0.0 ? volume : (out.area != 0.0 ? surface : linear);
        const gp_Pnt c = centre.CentreOfMass();
        out.centre[0] = c.X();
        out.centre[1] = c.Y();
        out.centre[2] = c.Z();

        fillRadius(raw, out);
        return out;
    });
}

Result<double> distanceBetween(const Shape& a, const Shape& b) {
    if (a.isNull() || b.isNull()) {
        return Error{ErrorCode::InvalidInput, "There is nothing here to measure between."};
    }
    return guard("measure a distance", [&]() -> double {
        BRepExtrema_DistShapeShape distance(occt(const_cast<Shape&>(a)),
                                            occt(const_cast<Shape&>(b)));
        if (!distance.IsDone() || distance.NbSolution() < 1) {
            throw std::runtime_error("the distance between these two could not be computed");
        }
        return distance.Value();
    });
}

}  // namespace cad::kernel
