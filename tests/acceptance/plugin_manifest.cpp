// The manifest parser, which is the first thing on the plugin path to touch a stranger's bytes.
//
// Everything here is about what happens BEFORE any plugin code runs. `dlopen` executes static
// initialisers, so the manifest is the only opportunity the host has to refuse a plugin without
// having already run part of it — which makes these the highest-consequence tests in the loader,
// and the reason the parser is a pure function over a string rather than something that reads a
// directory.

#include "../../abi/src/Manifest.h"

#include "cad/abi/cad_plugin_abi.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace cad;

namespace {

/// A manifest with everything a valid one needs, so a test can vary exactly one thing.
std::string valid(const std::string& extra = {}) {
    return "id = com.acme.sheetmetal\n"
           "name = Sheet Metal\n"
           "semver = 1.2.0\n"
           "abi_major = 1\n"
           "min_host_minor = 3\n"
           "library = sheetmetal\n" +
           extra;
}

abi::PluginManifest parsed(const std::string& text) {
    auto result = abi::parsePluginManifest(text, "/plugins/sheetmetal");
    REQUIRE(result);
    return result.value();
}

}  // namespace

TEST_CASE("a well-formed manifest parses", "[plugin][manifest]") {
    const auto m = parsed(valid("capabilities = filesystem, ui\n"));
    CHECK(m.id == "com.acme.sheetmetal");
    CHECK(m.displayName == "Sheet Metal");
    CHECK(m.semver == "1.2.0");
    CHECK(m.abiMajor == 1);
    CHECK(m.minHostMinor == 3);
    CHECK((m.requiredCaps & CAD_CAP_FILESYSTEM) != 0);
    CHECK((m.requiredCaps & CAD_CAP_UI) != 0);
    CHECK((m.requiredCaps & CAD_CAP_NETWORK) == 0);
}

TEST_CASE("comments and blank lines are ignored", "[plugin][manifest]") {
    const auto m = parsed("# a plugin\n\n   \n" + valid());
    CHECK(m.id == "com.acme.sheetmetal");
}

TEST_CASE("a manifest cannot name a library outside its own folder", "[plugin][manifest]") {
    // THE test in this file. A manifest is written by whoever shipped the plugin, so a path here
    // would turn "install this plugin" into "load that library from anywhere on the machine" —
    // and it would do so through a file the user was told is only metadata.
    for (const char* escape : {"../evil", "../../evil", "/usr/lib/evil", "sub/dir/evil",
                               "./../evil"}) {
        const auto result =
            abi::parsePluginManifest(valid() + "library = " + escape + "\n", "/plugins/x");
        INFO("library = " << escape);
        CHECK_FALSE(result);
    }
}

TEST_CASE("a bare library name gains the platform's suffix", "[plugin][manifest]") {
    // One manifest for five platforms. An explicit suffix is honoured as written, so a plugin
    // shipping differently-named binaries can still say so.
    const auto m = parsed(valid());
    CHECK(abi::libraryPath(m).filename().string() ==
          std::string("sheetmetal") + abi::sharedLibrarySuffix());

    const auto explicitly = parsed(valid() + "library = sheetmetal.custom\n");
    CHECK(abi::libraryPath(explicitly).filename().string() == "sheetmetal.custom");
}

TEST_CASE("the fields a host must have are required", "[plugin][manifest]") {
    // Each of these is something the host needs BEFORE loading, so a manifest without it cannot be
    // acted on and must be refused rather than defaulted. A defaulted abi_major in particular
    // would mean guessing which generation a plugin was built for.
    CHECK_FALSE(abi::parsePluginManifest("abi_major = 1\nlibrary = x\n", "/p"));         // no id
    CHECK_FALSE(abi::parsePluginManifest("id = a.b\nlibrary = x\n", "/p"));              // no abi
    CHECK_FALSE(abi::parsePluginManifest("id = a.b\nabi_major = 1\n", "/p"));            // no lib
}

TEST_CASE("a version field that is not a number is refused, not truncated", "[plugin][manifest]") {
    // `from_chars` reports trailing junk, and that matters: a parser that read "1x" as 1 would
    // load a plugin under a generation nobody wrote down.
    CHECK_FALSE(abi::parsePluginManifest(
        "id = a.b\nlibrary = x\nabi_major = 1x\n", "/p"));
    CHECK_FALSE(abi::parsePluginManifest(
        "id = a.b\nlibrary = x\nabi_major = 1\nmin_host_minor = 3 4\n", "/p"));
    CHECK_FALSE(abi::parsePluginManifest(
        "id = a.b\nlibrary = x\nabi_major = -1\n", "/p"));
}

TEST_CASE("an unknown capability is refused but an unknown key is not", "[plugin][manifest]") {
    // These differ on purpose. A capability this host has never heard of cannot be shown to a user
    // accurately, and silently dropping it would display a permission list that is not what the
    // plugin asked for. An unknown KEY is a newer plugin carrying a field this host predates, and
    // refusing that would make the manifest format non-additive — the exact property the ABI
    // itself is built to have.
    CHECK_FALSE(abi::parsePluginManifest(valid("capabilities = gpu\n"), "/p"));

    const auto m = parsed(valid("future_field = whatever\nanother = 12\n"));
    CHECK(m.id == "com.acme.sheetmetal");
}

TEST_CASE("a line without a separator is refused", "[plugin][manifest]") {
    CHECK_FALSE(abi::parsePluginManifest(valid("garbage line\n"), "/p"));
}

TEST_CASE("no input crashes the manifest parser", "[plugin][manifest]") {
    // The manifest is a file from a stranger, so the parser gets the same treatment as the DXF
    // reader: whatever it decides, it must decide it without crashing. Cheap structural sweep
    // rather than a substitute for a fuzzer — its value is that it runs every build.
    std::uint64_t state = 0x243F6A8885A308D3ull;
    const auto next = [&state] {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    };
    const std::string seed = valid("capabilities = filesystem\n# comment\n");
    for (int i = 0; i < 2000; ++i) {
        std::string bytes = seed;
        for (int edit = 0; edit < static_cast<int>(next() % 6) + 1 && !bytes.empty(); ++edit) {
            switch (next() % 3) {
                case 0: bytes[next() % bytes.size()] = static_cast<char>(next() % 256); break;
                case 1: bytes.resize(next() % bytes.size()); break;
                default:
                    bytes.insert(bytes.begin() + static_cast<std::ptrdiff_t>(next() % bytes.size()),
                                 static_cast<char>(next() % 256));
                    break;
            }
        }
        // The contract is "returns", not "succeeds".
        const auto result = abi::parsePluginManifest(bytes, "/p");
        if (result) {
            // Anything it DID accept must still be safe to act on -- which is the whole point.
            const auto path = abi::libraryPath(result.value());
            CHECK(path.parent_path() == std::filesystem::path("/p"));
        }
    }
}
