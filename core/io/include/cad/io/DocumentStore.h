#pragma once

#include "cad/document/Document.h"
#include "cad/kernel/Result.h"

#include <filesystem>
#include <string>

/// Reading and writing the NATIVE document format (ADR 0003): one SQLite file per document.
///
/// Distinct from `Format.h`, and the split is deliberate:
///
///   * `Format.h` / `FormatRegistry` is for FOREIGN formats — STEP, IGES, STL. Those carry
///     geometry and nothing else, so importing one produces a shape that becomes a feature.
///   * This is for OUR format, which carries the FEATURE TREE. Opening it restores the
///     parametric model — the properties, the references, the history of how the part was built.
///
/// A STEP file of a bracket and a .vpart of the same bracket are not the same information, and
/// conflating the two paths is how an application ends up unable to reopen its own documents
/// without losing everything that made them editable.
///
/// What is stored: objects, their types, and their properties. What is NOT stored: computed
/// shapes. Those are rebuilt by the recompute engine on open, and the content-addressed cache
/// (ADR 0004) usually has them already. That keeps files small and makes them immune to a
/// kernel upgrade changing tessellation or B-rep output. The cost is that opening a large
/// assembly is O(recompute); when that hurts, the fix is caching outputs in the file, and the
/// schema is versioned so that can be added without breaking existing documents.
namespace cad::io {

/// Schema version written into every file. Bump on any incompatible change, and teach `load` to
/// migrate — never to reject a file it could have read.
///
/// 2: properties carry the expression that produced them, and the display unit it was typed in.
///    Version 1 files still open: the loader asks the file which columns it has rather than
///    trusting this number, so a document written before expressions existed simply has none.
constexpr int kDocumentSchemaVersion = 2;

/// What a document file says about itself, readable without loading the model.
///
/// Cheap on purpose: the Home page lists recent documents and must not open and recompute every
/// one of them to show a name and a kind.
struct DocumentInfo {
    int schemaVersion = 0;

    /// Which naming scheme wrote this file's element references. See naming::kNamingSchemeVersion.
    ///
    /// Zero means the file predates the stamp, so nothing can be said about it -- which is exactly
    /// the situation the stamp exists to stop happening again.
    int namingSchemeVersion = 0;
    std::string kind;          ///< "Part", "Assembly", ... — matches app::DocumentKind's names
    std::string application;   ///< which build wrote it, for bug reports
    std::size_t objectCount = 0;
};

/// Writes `document` to `path`, replacing whatever is there.
///
/// Atomic from the caller's point of view: the write goes to a temporary beside the target and is
/// renamed over it only after it commits. A crash halfway through a save must not leave a
/// truncated file where the user's work used to be — losing an edit is recoverable, losing the
/// document is not.
kernel::Result<void> saveDocument(const document::Document&, const std::filesystem::path&,
                                 const std::string& kind = "Part");

/// Reads a document. The result is UNCOMPUTED — every object's output is null until the caller
/// runs the recompute engine over it.
[[nodiscard]] kernel::Result<document::Document> loadDocument(const std::filesystem::path&);

/// Reads only the header. Does not touch the objects.
[[nodiscard]] kernel::Result<DocumentInfo> readDocumentInfo(const std::filesystem::path&);

/// The extension for a document kind, without the dot's ambiguity: ".vpart", ".vasm", ...
[[nodiscard]] std::string extensionForKind(const std::string& kind);

}  // namespace cad::io
