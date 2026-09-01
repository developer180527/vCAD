/// Everything that crosses the filesystem: save, open, import and export.
///
/// Split out of Controller.cpp, which had reached 2574 lines. The class is unchanged --
/// these are the same methods in the same order, moved verbatim into a file named for what
/// they do, so the system can be read one concern at a time.

#include "Internal.h"

#include "cad/io/Format.h"
#include "cad/kernel/Primitives.h"

#include "cad/render/MetalSurface.h"

#include "cad/io/DocumentStore.h"
#include "cad/sketch/Sketch.h"

#include "cad/units/Units.h"

#include <sstream>
#include <tuple>

#include <algorithm>
#include <chrono>


namespace cad::app {

std::vector<Controller::ExportFormat> Controller::exportFormats() {
    static const io::FormatRegistry registry = io::FormatRegistry::builtins();
    std::vector<ExportFormat> out;
    for (const io::IFormatProvider* provider : registry.all()) {
        const auto caps = provider->capabilities();
        if (!caps.write) continue;
        out.push_back(ExportFormat{provider->id(), provider->displayName(),
                                   provider->extensions(), caps.solids});
    }
    return out;
}

bool Controller::exportDocument(const std::string& path) {
    if (path.empty()) {
        status("No file name given.");
        return false;
    }

    // The VISIBLE bodies, so the file matches the screen. A Box consumed by a Fillet is still in
    // the document; writing it too would put the un-filleted block in the file beside the real
    // part, and the user would find out in whatever opened it.
    std::vector<kernel::Shape> shapes;
    const auto& doc = history_.current();
    for (const render::Placement& placement : placements_) {
        if (!placement.visible) continue;
        const auto object = doc.find(placement.object);
        if (!object || object->output() == nullptr) continue;
        shapes.push_back(object->output()->shape);
    }

    if (shapes.empty()) {
        status("There is nothing to export.");
        return false;
    }

    static const io::FormatRegistry registry = io::FormatRegistry::builtins();
    if (registry.forPath(path) == nullptr) {
        status("No exporter handles that file type.");
        return false;
    }

    auto combined = shapes.size() == 1 ? kernel::Result<kernel::Shape>{shapes.front()}
                                       : kernel::compound(shapes);
    if (!combined) {
        status(combined.error().message);
        return false;
    }

    auto written = io::exportFile(registry, path, combined.value());
    if (!written) {
        status(written.error().message);
        return false;
    }
    status("Exported " + path);
    return true;
}

std::uint64_t Controller::saveDigest() const noexcept {
    // Document::digest() covers ids, types and property values — deliberately NOT labels, because
    // it feeds the recompute cache key and renaming a feature must not invalidate cached geometry.
    // But a rename IS a change worth saving, so folding labels in here is the difference between
    // "close without saving" losing a rename and preserving it. Two digests with two jobs.
    std::uint64_t h = history_.current().digest();
    const auto& doc = history_.current();
    for (const auto id : doc.ids()) {
        const auto object = doc.find(id);
        if (!object) continue;
        for (const char c : object->label()) {
            h ^= static_cast<std::uint64_t>(c);
            h *= 1099511628211ULL;
        }
    }
    return h;
}

bool Controller::modified() const noexcept { return saveDigest() != savedDigest_; }

kernel::Result<void> Controller::saveTo(const std::filesystem::path& path,
                                       const std::string& kind) {
    auto r = io::saveDocument(history_.current(), path, kind);
    if (!r) return r.error();
    savedDigest_ = saveDigest();
    notifyDocument();   // the title bar's dirty marker is derived from modified()
    status("Saved " + path.filename().string());
    return {};
}

kernel::Result<void> Controller::loadFrom(const std::filesystem::path& path) {
    // Read the header BEFORE the model, so that a document written under an older naming scheme can
    // be reported as such. Its element references -- every fillet's edges, every hole's face -- were
    // derived by rules this build no longer uses, so some of them may resolve to nothing.
    //
    // Said out loud rather than left to be discovered. The alternative is a part that was finished
    // last week opening with red features and nothing about the user's model to explain why.
    const auto info = io::readDocumentInfo(path);

    auto loaded = io::loadDocument(path);
    if (!loaded) return loaded.error();

    // A fresh History rather than a commit. Recompute happens through refresh() below, which is
    // the same path every edit takes, so a document that opens with a failed feature reports it
    // exactly like one that acquired the failure interactively.
    history_ = document::History{std::move(loaded.value())};
    selection_.clear();
    refresh();
    savedDigest_ = saveDigest();
    fitView();

    if (info && info.value().namingSchemeVersion < naming::kNamingSchemeVersion) {
        status("Opened " + path.filename().string()
               + " — it was built with an older naming scheme, so some references to faces and "
                 "edges may need re-selecting.");
    } else {
        status("Opened " + path.filename().string());
    }
    return {};
}

kernel::Result<ObjectId> Controller::importFile(const std::filesystem::path& path) {
    auto [next, id] = history_.current().add("Import");
    const auto object = next.find(id);
    auto updated = object->withProperty("path", path.string());
    next = next.replace(std::make_shared<const document::ObjectData>(std::move(updated)));

    // Recompute BEFORE committing. An import that cannot be read must not land in history: the
    // user would get an undo step for a feature that never produced geometry, and every later
    // recompute would retry the same unreadable file.
    recompute::Engine engine(registry_, cache_);
    auto computed = engine.recompute(next);
    if (!computed) return computed.error();

    next = computed.value().first;
    const auto imported = next.find(id);
    if (!imported || imported->output() == nullptr) {
        return kernel::Error{kernel::ErrorCode::InvalidInput,
                             "That file could not be imported.",
                             path.string()};
    }

    history_.commit(std::move(next), "Import " + path.filename().string());
    selection_.clear();
    selection_.push_back(id);
    refresh();
    fitView();
    status("Imported " + path.filename().string());
    return id;
}

}  // namespace cad::app
