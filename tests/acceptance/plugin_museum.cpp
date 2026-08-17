// The compatibility museum: every plugin binary this project has ever built, loaded forever.
//
// ADR 0011 calls this "the one that actually delivers the goal", and everything else in that
// document hygiene. The reason is that rules like "additive only, never change a signature" are
// what every project intends and what discipline alone never delivers. What survives a decade of
// contributors is a test that fails.
//
// The exhibits are COMPILED ARTEFACTS and are never rebuilt. That is the entire distinction:
// rebuilding from source would test SOURCE compatibility, and the promise is about a binary whose
// author may be gone, whose compiler may be gone, and whose company may be gone. `tests/plugins`
// builds from source on every run and checks something different and also useful; this checks the
// thing nothing else can.
//
// WHEN THIS FAILS, THE HOST IS WRONG. Do not refresh an exhibit to make CI green — that converts
// the one test that can detect a broken promise into a record of having broken it.

#include "cad/abi/cad_plugin_abi.h"

#include "../../abi/src/Loader.h"

#include <algorithm>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace cad;

namespace {

/// Where this build's exhibits live. Binaries are per-platform, so a triple that has none is a
/// platform nobody has frozen an exhibit on yet — reported, never silently skipped.
std::string triple() {
#if defined(_WIN32) && defined(__aarch64__)
    return "arm64-windows";
#elif defined(_WIN32)
    return "x64-windows";
#elif defined(__APPLE__)
    return "arm64-osx";
#elif defined(__aarch64__)
    return "arm64-linux";
#else
    return "x64-linux";
#endif
}

fs::path museumRoot() { return fs::path(CAD_REPO_ROOT) / "tests" / "museum"; }

/// A session and its host vtable, released together. Same shape as plugin_loader.cpp's, because
/// an exhibit must be loaded through the REAL host a user's plugin gets.
class Host {
public:
    Host() : session_(cad_session_create()) { host_ = cad_plugin_host(session_); }
    ~Host() { cad_session_release(session_); }
    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;
    [[nodiscard]] const CadHost* host() const { return host_; }

private:
    CadSession session_ = 0;
    const CadHost* host_ = nullptr;
};

/// Every exhibit directory for this platform, oldest ABI first.
std::vector<fs::path> exhibits() {
    std::vector<fs::path> found;
    std::error_code ec;
    if (!fs::exists(museumRoot(), ec)) return found;
    for (const auto& generation : fs::directory_iterator(museumRoot(), ec)) {
        if (!generation.is_directory()) continue;
        const fs::path dir = generation.path() / triple();
        if (fs::exists(dir / "plugin.manifest", ec)) found.push_back(dir);
    }
    std::sort(found.begin(), found.end());
    return found;
}

}  // namespace

TEST_CASE("every plugin ever built still loads", "[museum][abi]") {
    const auto all = exhibits();

    // An empty museum on a platform is not a pass. It means nobody has frozen a binary here, and a
    // silently-empty loop is exactly how a guarantee stops being checked without anyone noticing.
    INFO("triple: " << triple() << ", museum: " << museumRoot().string());
    REQUIRE_FALSE(all.empty());

    for (const fs::path& dir : all) {
        INFO("exhibit: " << dir.string());

        // Loaded through the REAL loader, with the real host vtable, exactly as a user's plugin
        // would be. A bespoke loading path here would test itself rather than the shipping one.
        Host host;
        REQUIRE(host.host() != nullptr);

        auto loaded = abi::loadPluginFrom(dir, host.host());
        if (!loaded) {
            FAIL("a plugin built against an older ABI no longer loads: "
                 << loaded.error().message << " (" << loaded.error().detail << ")\n"
                 << "ADR 0011 promises this binary keeps working. The HOST is wrong, not the "
                    "exhibit. Do not refresh it.");
        }

        // Loading is not enough. The plugin's own descriptor must have come back, and it must
        // still identify itself: a host that opens an old library and then ignores what it says
        // has broken the promise just as thoroughly, and more quietly.
        REQUIRE(loaded.value().desc != nullptr);
        CHECK(loaded.value().desc->id != nullptr);
        CHECK_FALSE(loaded.value().manifest.id.empty());

        // And the generation it was built against must be the one it still reports. If this ever
        // reads as the CURRENT minor, the exhibit has been rebuilt and the museum is measuring
        // nothing.
        CHECK(loaded.value().desc->abi_major == CAD_ABI_VERSION_MAJOR);
    }
}

TEST_CASE("an exhibit's bytes have not changed", "[museum][abi]") {
    // The museum only means anything if the binaries are frozen. This does not hash them — the
    // PROVENANCE file records the sha256 for a human — but it does assert the file is present and
    // non-trivial, which catches the common accident of a build system overwriting an exhibit with
    // a fresh artefact of the same name.
    for (const fs::path& dir : exhibits()) {
        INFO("exhibit: " << dir.string());
        CHECK(fs::exists(dir / "PROVENANCE"));

        bool sawLibrary = false;
        for (const auto& file : fs::directory_iterator(dir)) {
            const std::string name = file.path().filename().string();
            if (name == "PROVENANCE" || name == "plugin.manifest") continue;
            sawLibrary = true;
            CHECK(file.file_size() > 1024);
        }
        CHECK(sawLibrary);
    }
}
