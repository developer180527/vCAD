#include "cad/kernel/Healing.h"

#include "cad/kernel/Guard.h"
#include "cad/kernel/internal/Occt.h"

#include <BRep_Builder.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <TopoDS_Shell.hxx>
#include <TopoDS_Solid.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <ShapeFix_Shape.hxx>
#include <ShapeFix_Wireframe.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>

#include <sstream>

namespace cad::kernel {
namespace {

int countInvalid(const TopoDS_Shape& shape, TopAbs_ShapeEnum type) {
    TopTools_IndexedMapOfShape map;
    TopExp::MapShapes(shape, type, map);
    int bad = 0;
    for (int i = 1; i <= map.Extent(); ++i) {
        if (!BRepCheck_Analyzer(map(i)).IsValid()) ++bad;
    }
    return bad;
}

HealingReport describe(const TopoDS_Shape& shape) {
    HealingReport r;
    r.wasValid = BRepCheck_Analyzer(shape).IsValid() == Standard_True;
    r.isValidNow = r.wasValid;
    if (!r.wasValid) {
        r.invalidFaces = countInvalid(shape, TopAbs_FACE);
        r.invalidEdges = countInvalid(shape, TopAbs_EDGE);
        r.structuralDefect = (r.invalidFaces == 0 && r.invalidEdges == 0);
    }
    return r;
}

int faceCount(const TopoDS_Shape& s) {
    TopTools_IndexedMapOfShape map;
    TopExp::MapShapes(s, TopAbs_FACE, map);
    return map.Extent();
}

}  // namespace

std::string HealingReport::summary() const {
    std::ostringstream os;
    if (wasValid) {
        os << "Geometry is sound; no repairs needed.";
        return os.str();
    }
    const char* verb = isValidNow ? "Repaired" : "Could not fully repair";
    if (structuralDefect) {
        // Every face and edge checks out individually; the assembly of them does not.
        os << verb << " the shape's structure (the surfaces did not form a closed solid).";
    } else {
        os << verb << " " << invalidFaces << " face" << (invalidFaces == 1 ? "" : "s")
           << " and " << invalidEdges << " edge" << (invalidEdges == 1 ? "" : "s") << ".";
    }
    for (const auto& a : actions) os << " " << a;
    return os.str();
}

Result<HealingReport> inspect(const Shape& shape) {
    if (shape.isNull()) {
        return Error{ErrorCode::InvalidInput, "Cannot inspect an empty shape."};
    }
    return guard("inspect", [&] { return describe(occt(const_cast<Shape&>(shape))); });
}

Result<HealingReport> heal(Shape& inOut, const HealingOptions& options) {
    if (inOut.isNull()) {
        return Error{ErrorCode::InvalidInput, "Cannot heal an empty shape."};
    }

    return guard("heal", [&] {
        HealingReport report = describe(occt(inOut));
        if (report.wasValid && !options.unifySameDomain) {
            return report;   // nothing to do; do not touch the shape
        }

        TopoDS_Shape current = occt(inOut);
        const int facesBefore = faceCount(current);

        // 1. Sew first. Gaps between faces make everything downstream worse, and sewing is
        //    the fix most often needed by STEP/IGES from other systems.
        if (options.sewFreeEdges && !report.wasValid) {
            BRepBuilderAPI_Sewing sewing(options.tolerance * 100.0);
            sewing.Add(current);
            sewing.Perform();
            const TopoDS_Shape sewed = sewing.SewedShape();
            if (!sewed.IsNull()) {
                const int free = sewing.NbFreeEdges();
                current = sewed;
                report.changed = true;
                std::ostringstream os;
                os << "Sewed the surfaces together (" << free << " edge"
                   << (free == 1 ? "" : "s") << " were left open).";
                report.actions.push_back(os.str());
            }
        }

        // 2. The general fixer: face orientation, wire order, missing p-curves, tolerances.
        if (!report.wasValid) {
            ShapeFix_Shape fixer(current);
            fixer.SetPrecision(options.tolerance);
            fixer.SetMaxTolerance(options.tolerance * 1000.0);
            if (!options.fixFaceOrientation) {
                fixer.FixSolidTool()->FixShellOrientationMode() = 0;
            }
            fixer.Perform();
            if (!fixer.Shape().IsNull()) {
                current = fixer.Shape();
                report.changed = true;
                report.actions.emplace_back("Corrected face orientations and tolerances.");
            }
        }

        // 3. Degenerate and tiny edges, which break booleans and filleting later.
        if (options.fixSmallEdges && !report.wasValid) {
            Handle(ShapeFix_Wireframe) wf = new ShapeFix_Wireframe(current);
            wf->SetPrecision(options.tolerance);
            wf->ModeDropSmallEdges() = Standard_True;
            if (wf->FixSmallEdges() == Standard_True) {
                report.actions.emplace_back("Removed edges too short to be meaningful.");
                report.changed = true;
            }
            if (wf->FixWireGaps() == Standard_True) {
                report.actions.emplace_back("Closed gaps in face boundaries.");
                report.changed = true;
            }
            if (!wf->Shape().IsNull()) current = wf->Shape();
        }

        // 4. Optional, and off by default: this is a modelling decision, not a repair.
        //    It changes topology and therefore element names.
        if (options.unifySameDomain) {
            ShapeUpgrade_UnifySameDomain unifier(current, true, true, false);
            unifier.Build();
            if (!unifier.Shape().IsNull()) {
                current = unifier.Shape();
                const int after = faceCount(current);
                if (after != facesBefore) {
                    report.changed = true;
                    std::ostringstream os;
                    os << "Merged coplanar faces (" << facesBefore << " -> " << after << ").";
                    report.actions.push_back(os.str());
                }
            }
        }

        occt(inOut) = current;
        report.isValidNow = BRepCheck_Analyzer(current).IsValid() == Standard_True;
        return report;
    });
}

Result<Shape> makeOpenShellSolid(const Shape& source) {
    if (source.isNull()) {
        return Error{ErrorCode::InvalidInput, "Cannot build a shell from an empty shape."};
    }
    return guard("makeOpenShellSolid", [&] {
        TopTools_IndexedMapOfShape faces;
        TopExp::MapShapes(occt(const_cast<Shape&>(source)), TopAbs_FACE, faces);
        if (faces.Extent() < 2) {
            throw std::runtime_error("need at least two faces to make an open shell");
        }

        BRep_Builder builder;
        TopoDS_Shell shell;
        builder.MakeShell(shell);
        // Drop the last face: the shell no longer closes, so the solid is invalid.
        for (int i = 1; i < faces.Extent(); ++i) {
            builder.Add(shell, faces(i));
        }
        TopoDS_Solid solid;
        builder.MakeSolid(solid);
        builder.Add(solid, shell);
        return wrap(solid);
    });
}

}  // namespace cad::kernel
