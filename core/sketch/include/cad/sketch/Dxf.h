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
/// Reading is done by the Rust parser in `rust/cad-parse`, because a DXF is bytes from a stranger
/// and this is the one surface in the application where that is true. Vendored dime (BSD-3-Clause)
/// remains as the fallback for builds without a Rust toolchain; see COPYRIGHT.md for why neither
/// is libdxfrw. Both readers feed the same sketch-building pass, so which one ran changes the
/// safety properties and nothing about the result.
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

    /// Entities the file described but could not be read: a required field missing, or a
    /// coordinate that was not a finite number.
    ///
    /// Distinct from `degenerate`, and the distinction is the user's, not the parser's.
    /// `degenerate` is geometry the file stated correctly and we chose not to keep — a zero-length
    /// line is a real entity we have a policy about. This is geometry the file failed to state at
    /// all, so something was lost that nobody intended to lose. One is a setting the user can turn
    /// off; the other is a reason to go back to whoever sent the file.
    ///
    /// Counted rather than fatal. A file with one bad entity in a thousand should still give the
    /// user the other nine hundred and ninety-nine — but silently, it would give them a profile
    /// with a gap in it and no indication of why.
    std::size_t malformed = 0;

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

struct DxfExportOptions {
    /// Divides every coordinate on the way out, inverting DxfImportOptions::scale. So importing at
    /// 25.4 and exporting at 25.4 returns the original numbers.
    double scale = 1.0;

    /// Construction geometry is written to its own layer rather than dropped, so a round trip
    /// through another application preserves it — and our own importer maps that layer back.
    bool includeConstruction = true;
    std::string constructionLayer = "construction";
    std::string profileLayer = "0";
};

/// Writes a sketch as DXF R12 ASCII.
///
/// Written directly rather than through dime, and the reason is worth recording: dime reads DXF
/// well, but building a model from nothing through its API means assembling header variables,
/// layer tables and section objects by hand against sparse documentation. R12 output is a few dozen
/// group-code pairs and a published format, so a direct writer is less code, has no dependency
/// behaviour to discover, and is exactly as reusable.
///
/// It is also self-checking: the importer above shares no code with this writer, so an
/// export/import round trip is verified by an INDEPENDENT implementation rather than by the same
/// code twice — which is the one property a hand-rolled writer normally lacks. That still holds
/// with the Rust reader, which was written from the format rather than from this file.
///
/// R12 specifically, because it is the most widely readable DXF version in existence. Nothing here
/// needs a later revision, and every CAM, laser and plotter toolchain accepts it.
///
/// Note what a DXF cannot carry: CONSTRAINTS. Exporting a fully constrained sketch and reading it
/// back gives geometry at the solved positions with no relationships — the parametric intent is
/// lost. That is the format, not the writer. Use the native document format to keep a sketch
/// editable.
kernel::Result<void> exportDxf(const Sketch&, const std::filesystem::path&,
                              const DxfExportOptions& = {});

/// Reads `path` into a new sketch. The sketch is UNSOLVED and unconstrained.
///
/// PARTIAL IMPORTS. A file with some unreadable entities imports the rest and reports the loss in
/// `DxfImportReport::malformed`. This is a change of policy from the dime-based reader, which
/// refused any file it could not read completely, and the argument it was refused on is worth
/// recording because it was a good one: half a profile looks exactly like a whole profile, and the
/// user has no way to tell which half is missing. What changed is not the judgement but the
/// counter — there is now a number saying how much was lost, surfaced in `summary()` and logged at
/// WARNING. With that, a partial import is an informed one, and refusing a thousand-entity drawing
/// over a single corrupt record is the worse failure.
///
/// PRECISION. The Rust reader parses coordinates as `f64`, so what the file states is what the
/// sketch gets. The dime FALLBACK does not: dime declares `typedef float dxfdouble` and holds DXF
/// coordinates in SINGLE precision, its double variant commented out upstream, so that path
/// carries roughly 7 significant digits however precisely the file was written. Our writer emits
/// 17 either way, because the loss belonged to a reader rather than to the format.
///
/// A round-trip test must therefore assert a relative tolerance near 1e-6 to hold in BOTH
/// configurations, and must not be tightened to what the Rust reader alone can deliver — doing so
/// turns `-DCAD_USE_RUST_DXF=OFF` into a failing build for a reason that has nothing to do with
/// the change under test. Note also that values exactly representable in float32 (40, 25, 12.5,
/// and every other small dyadic rational) survive dime bit-for-bit, which makes a tight assertion
/// pass by accident and look like a guarantee it is not.
///
[[nodiscard]] kernel::Result<Sketch> importDxf(const std::filesystem::path& path,
                                              const DxfImportOptions& options = {},
                                              DxfImportReport* report = nullptr);

}  // namespace cad::io
