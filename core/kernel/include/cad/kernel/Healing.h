#pragma once

#include "cad/kernel/Result.h"
#include "cad/kernel/Shape.h"

#include <string>
#include <vector>

namespace cad::kernel {

/// What healing found and what it did about it. This is not diagnostics — it is the payload
/// of the import report the user can inspect, per docs/FORMATS.md rule 1: import never
/// silently discards.
struct HealingReport {
    bool wasValid = false;      ///< BRepCheck_Analyzer verdict before any fixing
    bool isValidNow = false;    ///< …and after
    bool changed = false;       ///< true if the shape was replaced

    /// Human-readable, in the order applied: "sewed 3 free edges", "fixed 2 face
    /// orientations". Shown to the user, so no OCCT class names.
    std::vector<std::string> actions;

    /// Counts before fixing, for the report UI.
    int invalidFaces = 0;
    int invalidEdges = 0;

    /// True when the shape as a whole is rejected but every individual face and edge is
    /// sound — an unclosed shell, a bad solid/shell nesting, wrong orientation. Common in
    /// foreign B-rep and NOT describable by the per-element counts, so the report has to
    /// say so explicitly rather than print "0 faces and 0 edges".
    bool structuralDefect = false;

    [[nodiscard]] bool succeeded() const noexcept { return isValidNow; }
    [[nodiscard]] std::string summary() const;
};

/// Options for the healing pipeline. Defaults are what import should use.
struct HealingOptions {
    double tolerance = 1e-7;
    bool fixSmallEdges = true;   ///< drop degenerate edges below tolerance
    bool fixFaceOrientation = true;
    bool sewFreeEdges = true;    ///< close small gaps between faces
    bool unifySameDomain = false;///< merge coplanar faces; OFF by default — it changes
                                 ///< topology and therefore element names, so it is a
                                 ///< modelling choice, not a repair
};

/// ShapeFix pipeline. Run on EVERY import: foreign B-rep is routinely invalid, and OCCT's
/// modelling algorithms will produce garbage or throw on invalid input rather than tell you.
///
/// IMPORTANT ORDERING: healing changes topology, so it must run BEFORE the element map is
/// built for an imported shape, never after. Healing a shape that already has names would
/// silently invalidate them — which is precisely the failure mode core/naming exists to
/// prevent. Import order is: read -> heal -> name.
Result<HealingReport> heal(Shape& inOut, const HealingOptions& options = {});

/// Non-mutating check, for the "is this shape sound?" question on its own.
Result<HealingReport> inspect(const Shape&);

/// TEST SUPPORT. Rebuilds a solid from `source`'s shell with one face dropped, producing a
/// shape BRepCheck rejects. Exists so the healing tests have a reproducible defect without
/// checking in a binary fixture; replace with a captured broken STEP file once M2 lands
/// real import.
Result<Shape> makeOpenShellSolid(const Shape& source);

}  // namespace cad::kernel
