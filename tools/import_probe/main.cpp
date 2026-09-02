/// What happens when vCAD meets files it did not write.
///
/// # Why this is a tool and not a test
///
/// The files it runs on cannot live in the repository. A supplier's STEP assembly is tens of
/// megabytes, most catalogue geometry may not be redistributed, and a useful corpus is thousands of
/// parts rather than three. So the corpus sits outside git and this walks whatever is pointed at.
///
/// # What it is actually asking
///
/// Not "does the file open" -- anything can open a file. Three questions, in order of how much they
/// matter:
///
///   1. **Does it read?** The format provider's answer, with the time it took.
///   2. **Can we NAME it?** Imported geometry has no construction history, so faces are identified
///      by measurement. This reports how much of a real part that actually identifies -- the number
///      nobody has ever measured for vCAD.
///   3. **Can we MODEL on it?** The one that decides whether an import is useful or merely
///      viewable. A reference to an imported face has to survive being read again, and has to
///      survive a feature being built on it. Both are checked.
///
/// Question 3 is where measurement-based naming gets tested for the first time, and it is the
/// difference between "vCAD imports STEP" and "an engineer can work with a supplier's part".

#include "assetlib/ddc.h"

#include "cad/io/Format.h"
#include "cad/kernel/Fillet.h"
#include "cad/kernel/Shape.h"
#include "cad/naming/ElementMap.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace {

/// How much work to do per file. A corpus contains one pathological file more often than not, and
/// a harness that cannot be told to skip it is a harness nobody runs twice.
struct Options {
    bool readOnly = false;        ///< read and measure; do not name
    bool stability = true;        ///< read a second time and compare names -- doubles the cost
    std::size_t maxFaces = 0;     ///< 0 = no cap; otherwise decline to name shapes larger than this
};

using Clock = std::chrono::steady_clock;
using cad::io::FormatRegistry;

double msSince(Clock::time_point from) {
    return std::chrono::duration<double, std::milli>(Clock::now() - from).count();
}

/// Everything learned about one file. Filled in as far as each file gets.
struct Result {
    std::string name;
    std::uintmax_t bytes = 0;

    std::string checksum;   ///< BLAKE3 of the bytes: the fixture's identity, not its filename
    std::string format;

    bool read = false;
    double readMs = 0.0;
    std::string readError;

    // RAW versus HEALED, kept apart on purpose.
    //
    // importFile reads and then heals, so what the application sees is always post-healing. That
    // hides the question that matters for interoperability: was the file we were GIVEN valid, or
    // did our healing quietly launder a defect in someone else's exporter? Measuring only the
    // healed shape means a translator can degrade for years without anyone noticing.
    bool rawRead = false;
    bool rawValid = false;      ///< BRepCheck_Analyzer on the shape as it arrived
    bool healedValid = false;   ///< …and after healing
    bool healingChanged = false;
    int invalidFaces = 0, invalidEdges = 0;
    bool structuralDefect = false;
    std::vector<std::string> healingActions;

    std::size_t solids = 0, faces = 0, edges = 0, vertices = 0;

    bool named = false;
    bool nameSkipped = false;
    bool meshSkipped = false;   ///< skipped because the format is mesh-only, not because of a cap
    double nameMs = 0.0;
    std::string nameError;
    std::size_t namedElements = 0, unnamed = 0, collisions = 0;
    /// Unnamed BY TYPE. A face nobody can identify is one thing; its edges and vertices going
    /// unnamed as a consequence is another, and the two want different fixes.
    std::size_t unnamedFaces = 0, unnamedEdges = 0, unnamedVertices = 0;

    /// A name taken from the first read still resolves in a second, independent read.
    bool stable = false;
    bool stableChecked = false;

    /// A fillet built on a named edge leaves the other faces still referenceable.
    bool modelable = false;
    bool modelableChecked = false;
    std::string modelError;
};

/// The name of every element, so two reads can be compared.
std::vector<cad::naming::ElementName> namesOf(const cad::naming::ElementMap& map) {
    auto names = map.allNames();
    std::sort(names.begin(), names.end());
    return names;
}

/// Reads and names one file the way the Import feature does -- best effort, because a file we did
/// not write may contain geometry no measurement can identify, and refusing it over that would stop
/// the user opening a part they only wanted to look at.
bool readAndName(const FormatRegistry& registry, const std::filesystem::path& path,
                 const Options& options, cad::kernel::Shape& shape, cad::naming::ElementMap& map,
                 Result& out) {
    const auto readStart = Clock::now();
    auto imported = cad::io::importFile(registry, path.string());
    out.readMs = msSince(readStart);
    if (!imported) {
        out.readError = imported.error().message;
        return false;
    }
    out.read = true;
    out.format = imported.value().report.format;
    const auto& healing = imported.value().report.healing;
    out.healedValid = healing.isValidNow;
    out.healingChanged = healing.changed;
    out.invalidFaces = healing.invalidFaces;
    out.invalidEdges = healing.invalidEdges;
    out.structuralDefect = healing.structuralDefect;
    out.healingActions = healing.actions;
    shape = imported.value().shape;

    out.solids = shape.subShapes(cad::kernel::ShapeType::Solid).size();
    out.faces = shape.subShapes(cad::kernel::ShapeType::Face).size();
    out.edges = shape.subShapes(cad::kernel::ShapeType::Edge).size();
    out.vertices = shape.subShapes(cad::kernel::ShapeType::Vertex).size();

    // A MESH is not named, and the probe has to make the same decision the Import feature makes or
    // it is measuring a program that does not exist. The format says which it is:
    // Capabilities::solids is "true B-rep; false means mesh-only".
    const auto* provider = registry.forPath(path.string());
    const bool mesh = provider != nullptr && !provider->capabilities().solids;

    if (options.readOnly || mesh || (options.maxFaces > 0 && out.faces > options.maxFaces)) {
        out.nameSkipped = true;
        out.meshSkipped = mesh;
        return false;
    }

    const auto nameStart = Clock::now();
    cad::naming::NamingContext naming(1, 0);
    auto named = naming.nameprimitive(shape, {},
                                      cad::naming::NamingContext::Naming::BestEffort);
    out.nameMs = msSince(nameStart);
    if (!named) {
        out.nameError = named.error().message;
        return false;
    }
    out.named = true;
    map = std::move(named.value());
    out.namedElements = map.size();
    const auto missing = map.unnamed(shape);
    out.unnamed = missing.size();
    for (const auto& element : missing) {
        switch (element.type()) {
            case cad::kernel::ShapeType::Face:   ++out.unnamedFaces; break;
            case cad::kernel::ShapeType::Edge:   ++out.unnamedEdges; break;
            case cad::kernel::ShapeType::Vertex: ++out.unnamedVertices; break;
            default: break;
        }
    }
    out.collisions = map.collisions().size();
    return true;
}

/// Reads WITHOUT healing and asks OCCT whether the file's own geometry is sound.
///
/// A separate read rather than a flag on the first one, because the healed shape is what the
/// application actually uses and must be measured as it will be used. This is the control.
void probeRaw(const FormatRegistry& registry, const std::filesystem::path& path, Result& out) {
    cad::io::ImportOptions options;
    options.heal = false;
    auto raw = cad::io::importFile(registry, path.string(), options);
    if (!raw) return;
    out.rawRead = true;
    out.rawValid = raw.value().shape.validate().ok();
}

Result probe(const FormatRegistry& registry, const std::filesystem::path& path,
             const Options& options) {
    Result out;
    out.name = path.filename().string();
    std::error_code ignored;
    out.bytes = std::filesystem::file_size(path, ignored);
    out.checksum = assetlib::blake3File(path);

    cad::kernel::Shape shape;
    cad::naming::ElementMap map;
    if (!readAndName(registry, path, options, shape, map, out)) return out;
    probeRaw(registry, path, out);

    // 2a. STABILITY. Read the same bytes again and check the names agree. If they do not, every
    // reference a user stores into this part is void the next time the document opens -- which is
    // worse than not being able to name it at all, because nothing says so.
    if (options.stability) {
        cad::kernel::Shape again;
        cad::naming::ElementMap againMap;
        Result second;
        if (readAndName(registry, path, options, again, againMap, second)) {
            out.stableChecked = true;
            out.stable = namesOf(map) == namesOf(againMap)
                         && cad::naming::contentHash(shape, map).hex()
                                == cad::naming::contentHash(again, againMap).hex();
        }
    }

    // 2b. MODELABILITY. Fillet a named edge and check that a reference taken BEFORE the feature
    // still resolves after it. This is the whole promise of the naming layer, asked of geometry
    // whose names came from measurement rather than from construction history.
    if (out.namedElements > 0) {
        // BOUNDED. Each attempt is a full fillet through OCCT, and on a part where most edges
        // cannot take one -- which is most parts, at a fixed radius chosen blind -- an unbounded
        // search costs hundreds of them. Eight is enough to answer "can we model on this at all"
        // and cheap enough to run over a corpus.
        constexpr int kMaxAttempts = 8;
        int attempts = 0;
        const auto edges = shape.subShapes(cad::kernel::ShapeType::Edge);
        for (const auto& edge : edges) {
            if (attempts >= kMaxAttempts) break;
            const auto edgeName = map.nameOf(edge);
            if (!edgeName) continue;

            // A witness taken before the edit: some other element we will look for afterwards.
            std::optional<cad::naming::ElementName> witness;
            for (const auto& face : shape.subShapes(cad::kernel::ShapeType::Face)) {
                if (auto n = map.nameOf(face)) { witness = n; break; }
            }
            if (!witness) break;

            ++attempts;
            auto rounded = cad::kernel::filletEdges(shape, {edge}, 0.1);
            if (!rounded) {
                out.modelError = rounded.error().message;
                continue;   // this edge cannot take a fillet; try another
            }
            cad::naming::NamingContext after(2, 0);
            const cad::kernel::Shape* base = &shape;
            const cad::naming::ElementMap* baseMap = &map;
            auto grown = after.propagate(rounded.value(), {base}, {baseMap});
            out.modelableChecked = true;
            if (!grown) {
                out.modelError = grown.error().message;
                break;
            }
            out.modelable = grown.value().resolve(*witness).has_value();
            out.modelError.clear();
            break;
        }
    }
    return out;
}

/// JSON string escaping. Enough for filenames, formats and OCCT's own messages -- which do
/// contain quotes and backslashes, so this is not optional.
std::string escaped(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (const char c : text) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) continue;   // control bytes: dropped
                out += c;
        }
    }
    return out;
}

/// One fixture as a JSON object.
///
/// # Why structured output rather than only a table
///
/// The table is for a person reading one run. This is for comparing two RELEASES: a snapshot per
/// fixture, archived and diffed, so that a regression is visible even when the application still
/// opens the file successfully. "It still opens" is the weakest possible claim and the easiest to
/// keep making while the answer quietly degrades.
///
/// Keyed by CHECKSUM as well as name, because a corpus outside the repository can be re-downloaded,
/// re-exported, or replaced. A snapshot compared against a different file with the same name is
/// worse than no snapshot.
void writeJson(std::FILE* out, const Result& r, bool first) {
    std::fprintf(out, "%s\n    {\n", first ? "" : ",");
    std::fprintf(out, "      \"file\": \"%s\",\n", escaped(r.name).c_str());
    std::fprintf(out, "      \"checksum\": \"%s\",\n", escaped(r.checksum).c_str());
    std::fprintf(out, "      \"bytes\": %ju,\n", static_cast<std::uintmax_t>(r.bytes));
    std::fprintf(out, "      \"format\": \"%s\",\n", escaped(r.format).c_str());
    std::fprintf(out, "      \"read\": %s,\n", r.read ? "true" : "false");
    if (!r.readError.empty()) {
        std::fprintf(out, "      \"readError\": \"%s\",\n", escaped(r.readError).c_str());
    }
    std::fprintf(out,
                 "      \"validity\": { \"rawRead\": %s, \"rawValid\": %s, "
                 "\"healedValid\": %s, \"healingChanged\": %s, \"invalidFaces\": %d, "
                 "\"invalidEdges\": %d, \"structuralDefect\": %s },\n",
                 r.rawRead ? "true" : "false", r.rawValid ? "true" : "false",
                 r.healedValid ? "true" : "false", r.healingChanged ? "true" : "false",
                 r.invalidFaces, r.invalidEdges, r.structuralDefect ? "true" : "false");
    std::fprintf(out,
                 "      \"counts\": { \"solids\": %zu, \"faces\": %zu, \"edges\": %zu, "
                 "\"vertices\": %zu },\n",
                 r.solids, r.faces, r.edges, r.vertices);
    std::fprintf(out,
                 "      \"naming\": { \"named\": %s, \"mesh\": %s, \"elements\": %zu, "
                 "\"unnamedFaces\": %zu, \"unnamedEdges\": %zu, \"unnamedVertices\": %zu, "
                 "\"collisions\": %zu, \"stable\": %s, \"modelable\": %s },\n",
                 r.named ? "true" : "false", r.meshSkipped ? "true" : "false", r.namedElements,
                 r.unnamedFaces, r.unnamedEdges, r.unnamedVertices, r.collisions,
                 r.stableChecked ? (r.stable ? "true" : "false") : "null",
                 r.modelableChecked ? (r.modelable ? "true" : "false") : "null");
    // Timings last, and named as such: they are the one part of a snapshot that legitimately
    // differs between machines, so a diff tool can be told to ignore them.
    std::fprintf(out, "      \"timingMs\": { \"read\": %.0f, \"name\": %.0f }\n    }",
                 r.readMs, r.nameMs);
}

const char* mark(bool ok, bool checked = true) {
    if (!checked) return " - ";
    return ok ? " ok" : "  X";
}

}   // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2) {
        std::printf("usage: vcad_import_probe [options] <directory-or-file> [more...]\n"
                    "  --read-only        read and measure; do not name\n"
                    "  --no-stability     skip the second read (halves the cost)\n"
                    "  --max-faces N      decline to name shapes with more than N faces\n"
                    "  --json PATH        write a structured snapshot, for diffing releases\n");
        return 2;
    }

    Options options;
    std::string jsonPath;
    std::vector<std::string> inputs;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--read-only") options.readOnly = true;
        else if (arg == "--no-stability") options.stability = false;
        else if (arg == "--json" && i + 1 < argc) jsonPath = argv[++i];
        else if (arg == "--max-faces" && i + 1 < argc) {
            options.maxFaces = static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
        } else {
            inputs.push_back(arg);
        }
    }

    const auto registry = FormatRegistry::builtins();
    const std::vector<std::string> extensions = registry.readableExtensions();

    std::vector<std::filesystem::path> files;
    for (const auto& input : inputs) {
        const std::filesystem::path root(input);
        if (std::filesystem::is_regular_file(root)) {
            files.push_back(root);
            continue;
        }
        std::error_code ignored;
        for (auto it = std::filesystem::recursive_directory_iterator(root, ignored);
             it != std::filesystem::recursive_directory_iterator(); ++it) {
            if (!it->is_regular_file()) continue;
            std::string ext = it->path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (std::find(extensions.begin(), extensions.end(), ext) != extensions.end()) {
                files.push_back(it->path());
            }
        }
    }
    std::sort(files.begin(), files.end());

    if (files.empty()) {
        std::printf("no files this build can read under those paths\n");
        return 1;
    }

    std::printf("%-40s %7s %6s %6s %7s %7s %7s %5s %5s %5s %6s\n", "file", "KB", "read", "name",
                "faces", "unF", "unE/unV", "coll", "stbl", "mdl", "ms");
    std::printf("%s\n", std::string(118, '-').c_str());

    std::size_t readOk = 0, namedOk = 0, fullyNamed = 0, stable = 0, modelable = 0,
                modelChecked = 0, withCollisions = 0;
    std::size_t totalFaces = 0, totalUnnamedFaces = 0, meshes = 0;
    std::size_t rawValid = 0, healedValid = 0, rescued = 0, rawChecked = 0;
    std::FILE* json = jsonPath.empty() ? nullptr : std::fopen(jsonPath.c_str(), "w");
    if (json != nullptr) std::fprintf(json, "{\n  \"fixtures\": [");
    bool firstRecord = true;

    for (const auto& path : files) {
        std::printf("%-40.40s ", path.filename().string().c_str());
        const Result r = probe(registry, path, options);
        char counts[32];
        std::snprintf(counts, sizeof(counts), "%zu/%zu", r.unnamedEdges, r.unnamedVertices);
        std::printf("%7ju %6s %6s %7zu %7zu %9s %5zu %5s %5s %6.0f\n",
                    static_cast<std::uintmax_t>(r.bytes / 1024), mark(r.read), mark(r.named),
                    r.faces, r.unnamedFaces, counts, r.collisions,
                    r.meshSkipped ? "mesh" : mark(r.stable, r.stableChecked),
                    r.meshSkipped ? "mesh" : mark(r.modelable, r.modelableChecked),
                    r.readMs + r.nameMs);
        // The file's OWN validity, versus ours after healing. Printed only when they differ or
        // when something is wrong, because "arrived valid, stayed valid" is the whole corpus most
        // days and a column of "ok ok" teaches nothing.
        if (r.rawRead && (!r.rawValid || !r.healedValid)) {
            std::printf("      valid: as-read %s, after healing %s%s%s\n",
                        r.rawValid ? "yes" : "NO", r.healedValid ? "yes" : "NO",
                        r.structuralDefect ? "  (structural defect)" : "",
                        r.healingChanged ? "  (healing changed the shape)" : "");
            if (r.invalidFaces > 0 || r.invalidEdges > 0) {
                std::printf("             %d invalid face(s), %d invalid edge(s)\n",
                            r.invalidFaces, r.invalidEdges);
            }
            for (const auto& action : r.healingActions) {
                std::printf("             healed: %s\n", action.c_str());
            }
        }
        if (r.readMs > 200.0 || r.nameMs > 200.0) {
            std::printf("      time: read %.0f ms, name %.0f ms%s\n", r.readMs, r.nameMs,
                        r.nameSkipped ? "  (naming skipped)" : "");
        }
        if (!r.read && !r.readError.empty()) std::printf("      read: %s\n", r.readError.c_str());
        if (r.read && !r.named && r.nameSkipped && !r.meshSkipped) {
            std::printf("      naming skipped\n");
        }
        if (r.read && !r.named && !r.nameError.empty()) {
            std::printf("      name: %s\n", r.nameError.c_str());
        }
        if (r.modelableChecked && !r.modelable && !r.modelError.empty()) {
            std::printf("      model: %s\n", r.modelError.c_str());
        }

        if (json != nullptr) {
            writeJson(json, r, firstRecord);
            firstRecord = false;
        }
        readOk += r.read ? 1 : 0;
        meshes += r.meshSkipped ? 1 : 0;
        namedOk += r.named ? 1 : 0;
        fullyNamed += (r.named && r.unnamed == 0) ? 1 : 0;
        stable += r.stable ? 1 : 0;
        modelable += r.modelable ? 1 : 0;
        modelChecked += r.modelableChecked ? 1 : 0;
        withCollisions += r.collisions > 0 ? 1 : 0;
        // Only files we actually NAMED. Counting a mesh's 37,817 faces as identified -- when
        // naming was deliberately skipped and `unnamedFaces` was therefore never computed -- turned
        // this line into 100.0%, which is the most flattering possible way to say nothing.
        if (r.named) {
            totalFaces += r.faces;
            totalUnnamedFaces += r.unnamedFaces;
        }
        if (r.rawRead) {
            ++rawChecked;
            rawValid += r.rawValid ? 1 : 0;
            healedValid += r.healedValid ? 1 : 0;
            rescued += (!r.rawValid && r.healedValid) ? 1 : 0;
        }
    }

    if (json != nullptr) {
        std::fprintf(json, "\n  ]\n}\n");
        std::fclose(json);
    }

    const auto pct = [&](std::size_t n) {
        return files.empty() ? 0.0 : 100.0 * static_cast<double>(n) / static_cast<double>(files.size());
    };
    std::printf("\n%zu files\n", files.size());
    std::printf("  read            %4zu  (%.0f%%)\n", readOk, pct(readOk));
    std::printf("  meshes          %4zu           read as geometry, deliberately not named\n",
                meshes);
    std::printf("  named           %4zu  (%.0f%%)   fully, no unnamed element: %zu\n", namedOk,
                pct(namedOk), fullyNamed);
    std::printf("  name-stable     %4zu  (%.0f%%)   two reads agree on every name\n", stable,
                pct(stable));
    std::printf("  modelable       %4zu  of %zu tried   a reference survives a feature\n",
                modelable, modelChecked);
    std::printf("  valid as read   %4zu of %zu   the file's own geometry, before we touched it\n",
                rawValid, rawChecked);
    std::printf("  valid healed    %4zu of %zu   of which %zu were rescued by healing\n",
                healedValid, rawChecked, rescued);
    std::printf("  with collisions %4zu           MUST be zero: a name meaning two elements\n",
                withCollisions);
    std::printf("  faces           %4zu identified of %zu in named files  (%.1f%%)\n",
                totalFaces - totalUnnamedFaces, totalFaces,
                totalFaces == 0 ? 0.0
                                : 100.0 * static_cast<double>(totalFaces - totalUnnamedFaces)
                                      / static_cast<double>(totalFaces));
    return 0;
}
