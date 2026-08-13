#pragma once

#include "cad/kernel/Result.h"
#include "cad/sketch/Sketch.h"

#include <filesystem>
#include <string>
#include <vector>

/// DXF import into a sketch.
///
/// The thing to understand before using this: **a DXF file has no constraints.** It is dumb
/// geometry — coordinates and nothing else. An imported sketch therefore arrives with its full
/// degrees of freedom and is not yet parametric: move one line and nothing follows it.
///
/// That is not a defect in the reader, it is what the format is. Making an import editable needs
/// constraint inference (shared endpoints -> Coincident, near-axis lines -> Horizontal/Vertical),
/// which is a separate step deliberately kept out of here: inference has to be tunable and
/// reviewable by the user, and silently guessing constraints during a file read is worse than not
/// guessing at all.
///
/// Reading is done by dime (BSD-3-Clause). See COPYRIGHT.md for why not libdxfrw.
namespace cad::io {

// Names from cad::sketch are used unqualified below: this header is about sketches, and
// sketch::Plane / sketch::Sketch read as noise when every declaration mentions them.
using sketch::Plane;
using sketch::Sketch;

struct DxfImportOptions {
    /// Which plane the DXF's 2D coordinates land on.
    Plane plane = Plane::XY;

    /// Multiplied into every coordinate.
    ///
    /// DXF records its units in the `$INSUNITS` header variable, but a very large share of real
    /// files in circulation leave it unset or wrong — CAM and laser-cutting exports especially. So
    /// this is explicit rather than inferred: the caller decides, and the UI should ask when it
    /// cannot tell. Guessing the scale of an imported profile is the kind of mistake that is not
    /// visible until something is machined at the wrong size.
    double scale = 1.0;

    /// Layers whose geometry becomes construction geometry rather than profile. Matched
    /// case-insensitively. `defpoints` is AutoCAD's convention for non-plotting reference geometry,
    /// so it is here by default.
    std::vector<std::string> constructionLayers{"construction", "defpoints"};

    /// Skip entities whose geometry is degenerate — zero-length lines, zero-radius circles. These
    /// are common in exported files and every one of them is a piece of geometry the solver would
    /// have to carry with no way to constrain it sensibly.
    bool dropDegenerate = true;
};

/// What the import found, including everything it could not represent.
///
/// Fidelity loss is reported rather than hidden. A profile that silently lost its curved segments
/// looks plausible and machines wrong.
struct DxfImportReport {
    std::size_t lines = 0;
    std::size_t arcs = 0;
    std::size_t circles = 0;
    std::size_t points = 0;
    std::size_t polylines = 0;          ///< converted to runs of lines/arcs
    std::size_t construction = 0;       ///< of the above, on a construction layer
    std::size_t degenerate = 0;         ///< dropped as zero-length/zero-radius

    /// Entity types present in the file that we do not import, with counts — "SPLINE x 4".
    /// Named so a user can tell whether the missing geometry mattered.
    std::vector<std::pair<std::string, std::size_t>> unsupported;

    /// Polyline bulges (arc segments inside a polyline) that were flattened to straight lines.
    /// A real fidelity loss, so it gets its own counter rather than hiding inside `polylines`.
    std::size_t flattenedBulges = 0;

    /// True if any geometry lay off the target plane and was projected onto it.
    bool projected = false;

    [[nodiscard]] std::size_t imported() const noexcept {
        return lines + arcs + circles + points;
    }
    /// A human-readable summary for the status bar.
    [[nodiscard]] std::string summary() const;
};

/// Reads `path` into a new sketch. The sketch is UNSOLVED and unconstrained.
[[nodiscard]] kernel::Result<Sketch> importDxf(const std::filesystem::path& path,
                                              const DxfImportOptions& options = {},
                                              DxfImportReport* report = nullptr);

}  // namespace cad::io
