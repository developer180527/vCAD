// The formats OCCT gives us without a paid add-on.
//
// Toolkit names are the 7.8+ TKDE* ones — TKDESTEP, not TKSTEP. Anything copied from an
// older example will not link. See docs/STACK.md.

#include "cad/io/Format.h"

#include "cad/kernel/Guard.h"
#include "cad/kernel/internal/Occt.h"

#include <BRepMesh_IncrementalMesh.hxx>
#include <IGESControl_Reader.hxx>
#include <IGESControl_Writer.hxx>
#include <Interface_Static.hxx>
#include <STEPControl_Reader.hxx>
#include <STEPControl_Writer.hxx>
#include <StlAPI_Reader.hxx>
#include <StlAPI_Writer.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>

#include <algorithm>
#include <sstream>

namespace cad::io {
namespace {

using kernel::Error;
using kernel::ErrorCode;

int countOf(const kernel::Shape& s, kernel::ShapeType t) {
    return static_cast<int>(s.subShapes(t).size());
}

void fillCounts(ImportReport& r, const kernel::Shape& s) {
    r.solids = static_cast<std::size_t>(countOf(s, kernel::ShapeType::Solid));
    r.shells = static_cast<std::size_t>(countOf(s, kernel::ShapeType::Shell));
    r.faces = static_cast<std::size_t>(countOf(s, kernel::ShapeType::Face));
}

/// Applies healing and records what it did, in the one place the ordering is guaranteed:
/// heal happens here, before the caller ever builds an element map.
kernel::Result<void> healInto(kernel::Shape& shape, const ImportOptions& options,
                              ImportReport& report) {
    if (!options.heal) return {};
    auto healed = kernel::heal(shape, options.healing);
    if (!healed) return healed.error();
    report.healing = healed.value();
    if (!report.healing.succeeded() && !report.healing.wasValid) {
        report.warnings.push_back(
            "The file's geometry is not fully valid and could not be completely repaired. "
            "Some operations on it may fail.");
    }
    return {};
}

// --- STEP ---------------------------------------------------------------------------------

class StepProvider final : public IFormatProvider {
public:
    std::string id() const override { return "step"; }
    std::string displayName() const override { return "STEP (AP203 / AP214 / AP242)"; }
    std::vector<std::string> extensions() const override {
        return {".step", ".stp"};
    }
    Capabilities capabilities() const override {
        // pmi is false and stays false until we actually read AP242 semantic PMI. Claiming
        // a capability we do not have is worse than not having it: the UI would stop
        // warning the user that their GD&T is about to be dropped.
        return {true, true, true, true, false, true, false};
    }

    kernel::Result<ImportResult> read(const std::string& path,
                                      const ImportOptions& options) override {
        return kernel::guard("STEP read", [&]() -> ImportResult {
            STEPControl_Reader reader;
            if (reader.ReadFile(path.c_str()) != IFSelect_RetDone) {
                throw std::runtime_error("the file could not be opened or is not valid STEP");
            }

            ImportResult out;
            out.report.format = displayName();
            out.report.sourcePath = path;

            // STEP declares its own units and OCCT applies them during transfer, so the
            // result is already in millimetres. Record what the file said so the report can
            // show it — a user importing a part that comes in 25.4x wrong needs to see
            // which unit was believed.
            if (const char* unit = Interface_Static::CVal("xstep.cascade.unit")) {
                out.report.declaredUnits = unit;
            }
            out.report.unitsWereAssumed = false;

            const int roots = reader.NbRootsForTransfer();
            if (roots == 0) throw std::runtime_error("the file contains no geometry");
            reader.TransferRoots();
            if (reader.NbShapes() == 0) {
                throw std::runtime_error("nothing in the file could be converted");
            }

            out.shape = kernel::wrap(reader.OneShape());
            fillCounts(out.report, out.shape);

            if (auto r = healInto(out.shape, options, out.report); !r) {
                throw std::runtime_error(r.error().message);
            }
            if (out.report.faces == 0) {
                out.report.unsupported.emplace_back(
                    "The file contained only non-surface data (points, curves or "
                    "annotations), which this version does not import.");
            }
            return out;
        });
    }

    kernel::Result<void> write(const std::string& path, const kernel::Shape& shape) override {
        if (shape.isNull()) {
            return Error{ErrorCode::InvalidInput, "There is no geometry to export."};
        }
        return kernel::guard("STEP write", [&] {
            STEPControl_Writer writer;
            if (writer.Transfer(kernel::occt(const_cast<kernel::Shape&>(shape)),
                                STEPControl_AsIs) != IFSelect_RetDone) {
                throw std::runtime_error("the geometry could not be converted to STEP");
            }
            if (writer.Write(path.c_str()) != IFSelect_RetDone) {
                throw std::runtime_error("the file could not be written");
            }
        });
    }
};

// --- IGES ---------------------------------------------------------------------------------

class IgesProvider final : public IFormatProvider {
public:
    std::string id() const override { return "iges"; }
    std::string displayName() const override { return "IGES (up to 5.3)"; }
    std::vector<std::string> extensions() const override { return {".iges", ".igs"}; }
    Capabilities capabilities() const override {
        return {true, true, true, false, false, true, false};
    }

    kernel::Result<ImportResult> read(const std::string& path,
                                      const ImportOptions& options) override {
        return kernel::guard("IGES read", [&]() -> ImportResult {
            IGESControl_Reader reader;
            if (reader.ReadFile(path.c_str()) != IFSelect_RetDone) {
                throw std::runtime_error("the file could not be opened or is not valid IGES");
            }
            ImportResult out;
            out.report.format = displayName();
            out.report.sourcePath = path;

            reader.TransferRoots();
            if (reader.NbShapes() == 0) {
                throw std::runtime_error("nothing in the file could be converted");
            }
            out.shape = kernel::wrap(reader.OneShape());
            fillCounts(out.report, out.shape);

            // IGES is overwhelmingly surfaces rather than solids, and a pile of unsewn
            // faces behaves very differently downstream. Say so rather than let the user
            // discover it when a boolean fails.
            if (out.report.solids == 0 && out.report.faces > 0) {
                out.report.warnings.emplace_back(
                    "This file contains surfaces rather than solids. Healing will try to "
                    "sew them; operations that need a solid may still fail.");
            }
            if (auto r = healInto(out.shape, options, out.report); !r) {
                throw std::runtime_error(r.error().message);
            }
            return out;
        });
    }

    kernel::Result<void> write(const std::string& path, const kernel::Shape& shape) override {
        if (shape.isNull()) {
            return Error{ErrorCode::InvalidInput, "There is no geometry to export."};
        }
        return kernel::guard("IGES write", [&] {
            IGESControl_Writer writer;
            writer.AddShape(kernel::occt(const_cast<kernel::Shape&>(shape)));
            writer.ComputeModel();
            if (!writer.Write(path.c_str())) {
                throw std::runtime_error("the file could not be written");
            }
        });
    }
};

// --- STL -----------------------------------------------------------------------------------

class StlProvider final : public IFormatProvider {
public:
    std::string id() const override { return "stl"; }
    std::string displayName() const override { return "STL (mesh)"; }
    std::vector<std::string> extensions() const override { return {".stl"}; }
    Capabilities capabilities() const override {
        // units=false is the important one: STL carries no unit declaration at all, which
        // is why ImportOptions::assumedUnits has no default.
        return {true, true, false, false, false, false, false};
    }

    kernel::Result<ImportResult> read(const std::string& path,
                                      const ImportOptions& options) override {
        return kernel::guard("STL read", [&]() -> ImportResult {
            ImportResult out;
            out.report.format = displayName();
            out.report.sourcePath = path;

            TopoDS_Shape shape;
            StlAPI_Reader reader;
            if (!reader.Read(shape, path.c_str()) || shape.IsNull()) {
                throw std::runtime_error("the file could not be read as STL");
            }
            out.shape = kernel::wrap(shape);
            fillCounts(out.report, out.shape);

            // STL has no units. The caller must have told us what to assume; record that we
            // assumed rather than knew, so the UI can prompt.
            auto factor = units::scaleToMillimetres(units::suffix(options.assumedUnits));
            out.report.scaleToMillimetres = factor.ok() ? factor.value() : 1.0;
            out.report.unitsWereAssumed = true;
            out.report.warnings.emplace_back(
                std::string("STL files do not record their units; this one was read as ") +
                units::suffix(options.assumedUnits) + ".");
            out.report.unsupported.emplace_back(
                "STL stores triangles only. There are no exact surfaces, no solids and no "
                "colours to import.");

            if (auto r = healInto(out.shape, options, out.report); !r) {
                throw std::runtime_error(r.error().message);
            }
            return out;
        });
    }

    kernel::Result<void> write(const std::string& path, const kernel::Shape& shape) override {
        if (shape.isNull()) {
            return Error{ErrorCode::InvalidInput, "There is no geometry to export."};
        }
        return kernel::guard("STL write", [&] {
            // Exporting an untessellated shape silently produces an empty file, which is a
            // miserable bug to chase. Mesh first, always.
            TopoDS_Shape& s = kernel::occt(const_cast<kernel::Shape&>(shape));
            BRepMesh_IncrementalMesh mesher(s, 0.01, Standard_False, 0.5, Standard_True);
            mesher.Perform();

            StlAPI_Writer writer;
            writer.ASCIIMode() = Standard_False;
            if (!writer.Write(s, path.c_str())) {
                throw std::runtime_error("the file could not be written");
            }
        });
    }
};

}  // namespace

std::vector<std::shared_ptr<IFormatProvider>> occtProviders() {
    return {
        std::make_shared<StepProvider>(),
        std::make_shared<IgesProvider>(),
        std::make_shared<StlProvider>(),
    };
}

}  // namespace cad::io
