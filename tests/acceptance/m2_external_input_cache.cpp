// The cache key must follow an imported file's CONTENTS, not its path.
//
// Import is the only built-in that reads something the document does not contain. Its cache key
// used to cover the `path` STRING, so editing the referenced file on disk and recomputing served
// the shape cached from the previous contents: silently wrong geometry, with no error anywhere and
// nothing on screen to suggest the model no longer matched the file it claimed to import.
//
// Written against Engine::cacheKeyOf directly rather than through the C ABI. The key is not part
// of the ABI and should not become part of it — ADR 0011 commits us to supporting every exported
// function for a decade, so adding one purely to make a test reachable from Rust would be paying a
// permanent cost for a temporary convenience. cacheKeyOf is documented as exposed for exactly this.

#include "cad/document/Document.h"
#include "cad/recompute/Engine.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

using namespace cad;

namespace {

/// A file in the temp directory that removes itself. The test writes real bytes because the whole
/// point is that the digest is taken from bytes on disk.
struct TempFile {
    std::filesystem::path path;

    explicit TempFile(const char* name)
        : path(std::filesystem::temp_directory_path() / name) {}
    ~TempFile() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }

    void write(const std::string& contents) const {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << contents;
    }
};

/// An Import object pointing at `path`, and the key the engine would give it.
std::uint64_t keyForImportOf(const std::filesystem::path& path) {
    document::Document doc;
    auto [next, id] = doc.add("Import");
    doc = next;
    const auto object = doc.find(id);
    doc = doc.replace(std::make_shared<const document::ObjectData>(
        object->withProperty("path", path.string())));

    const auto registry = recompute::FeatureRegistry::builtins();
    auto key = recompute::Engine::cacheKeyOf(doc, *doc.find(id), registry);
    REQUIRE(key);
    return key.value();
}

}  // namespace

TEST_CASE("an imported file's contents are part of the cache key", "[m2][cache]") {
    TempFile file("vcad_external_input_test.stl");

    file.write("solid a\nendsolid a\n");
    const std::uint64_t first = keyForImportOf(file.path);

    // Same path, different bytes. This is the case that was broken: nothing the document knows
    // about has changed, so without the file digest the key is identical and the stale cached
    // shape is returned.
    file.write("solid b\nfacet normal 0 0 1\nendfacet\nendsolid b\n");
    const std::uint64_t second = keyForImportOf(file.path);

    CHECK(first != second);
}

TEST_CASE("an unchanged imported file keeps its cache key", "[m2][cache]") {
    TempFile file("vcad_external_input_stable.stl");
    file.write("solid a\nendsolid a\n");

    const std::uint64_t first = keyForImportOf(file.path);
    const std::uint64_t second = keyForImportOf(file.path);

    // The other half of the property, and the one that makes the cache worth having: hashing the
    // contents must not make the key unstable. A key that changed on every read would turn every
    // recompute into a miss and quietly disable the cache — a "fix" that passes the test above.
    CHECK(first == second);
}

TEST_CASE("a missing imported file still produces a key", "[m2][cache]") {
    const auto missing =
        std::filesystem::temp_directory_path() / "vcad_definitely_not_here.stl";
    std::error_code ec;
    std::filesystem::remove(missing, ec);

    // Deliberately not an error. Refusing to build a key for an unreadable file would leave the
    // feature unable to recompute even to report its own failure, so a broken path would freeze
    // rather than fail legibly.
    const auto registry = recompute::FeatureRegistry::builtins();
    document::Document doc;
    auto [next, id] = doc.add("Import");
    doc = next;
    doc = doc.replace(std::make_shared<const document::ObjectData>(
        doc.find(id)->withProperty("path", missing.string())));

    CHECK(recompute::Engine::cacheKeyOf(doc, *doc.find(id), registry));
}
