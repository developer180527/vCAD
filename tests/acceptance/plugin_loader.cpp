// The loader, against a real compiled plugin.
//
// Every plugin test before this one called the host vtable directly from inside the test process.
// That exercises the CONTRACT and not the LOADING: no manifest was read, nothing was dlopen'd, no
// descriptor was checked against a manifest, and `cad_plugin_main` was never the entry point. This
// file is the first thing in the project to load a separately compiled plugin, which is what turns
// the ABI from a well-designed contract with zero clients into one with a client.

#include "../../abi/src/Loader.h"

#include "cad/abi/cad_plugin_abi.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <algorithm>
#include <string>

using namespace cad;

namespace {

/// A session and its host vtable, released together.
class Host {
public:
    Host() : session_(cad_session_create()) { host_ = cad_plugin_host(session_); }
    ~Host() { cad_session_release(session_); }
    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;

    [[nodiscard]] const CadHost* host() const { return host_; }
    [[nodiscard]] CadSession session() const { return session_; }

private:
    CadSession session_ = 0;
    const CadHost* host_ = nullptr;
};

std::filesystem::path demoDirectory() { return std::filesystem::path(CAD_TEST_PLUGIN_DIR) / "demo"; }

/// A copy of the demo plugin's directory with its manifest replaced, so a test can vary exactly
/// one line of a manifest that otherwise really does describe a real library.
std::filesystem::path stagedCopy(const std::string& name, const std::string& manifest) {
    const auto dir = std::filesystem::temp_directory_path() / ("cad-plugin-test-" + name);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    for (const auto& entry : std::filesystem::directory_iterator(demoDirectory())) {
        if (entry.path().filename() == "plugin.manifest") continue;
        std::filesystem::copy_file(entry.path(), dir / entry.path().filename(),
                                   std::filesystem::copy_options::overwrite_existing, ec);
    }
    std::ofstream(dir / "plugin.manifest") << manifest;
    return dir;
}

}  // namespace

TEST_CASE("a real plugin loads, registers, and computes", "[plugin][loader]") {
    Host host;
    REQUIRE(host.host() != nullptr);

    auto loaded = abi::loadPluginFrom(demoDirectory(), host.host());
    if (!loaded) {
        FAIL(loaded.error().message + " | " + loaded.error().detail);
    }
    REQUIRE(loaded);
    CHECK(loaded.value().manifest.id == "com.vcad.demo");
    CHECK(loaded.value().desc->abi_major == CAD_ABI_VERSION_MAJOR);

    // Registered during initialize, and usable immediately: add an object of the plugin's type
    // and recompute. This is the assertion the whole loader exists for -- a type that arrived
    // from a shared library on disk producing geometry in a document.
    CadObject object = 0;
    REQUIRE(cad_object_add(host.session(), "com.vcad.demo.Cube", &object) == CAD_OK);
    REQUIRE(cad_object_set_real(host.session(), object, "size", 12.0) == CAD_OK);

    CadRecomputeReport report{};
    REQUIRE(cad_recompute(host.session(), &report) == CAD_OK);
    INFO("failed=" << report.failed << " blocked=" << report.blocked
                   << " needs_plugin=" << report.needs_plugin);
    CHECK(report.failed == 0);
    CHECK(report.blocked == 0);
    CHECK(report.computed == 1);

    std::uint64_t faces = 0;
    REQUIRE(cad_object_face_count(host.session(), object, &faces) == CAD_OK);
    CHECK(faces == 6);
}

TEST_CASE("a plugin needing a newer host is refused before it is loaded", "[plugin][loader]") {
    // The forward direction, and it is refusal rather than compatibility. PLUGIN_CONTRACT.md §8
    // asks for this as its own test because the failure it prevents -- loading anyway and calling
    // a function pointer the old host never populated -- is the hardest possible crash to
    // attribute: it happens inside third-party code, at a stack depth that names the host.
    //
    // Refused from the MANIFEST, so the library is never opened and its static initialisers never
    // run. That is the whole reason the manifest exists.
    Host host;
    const auto dir = stagedCopy("future", "id = com.vcad.demo\nabi_major = 1\n"
                                          "min_host_minor = 9999\nlibrary = vcad_demo_plugin\n");
    const auto loaded = abi::loadPluginFrom(dir, host.host());
    REQUIRE_FALSE(loaded);
    INFO(loaded.error().message);
    CHECK(loaded.error().message.find("newer version") != std::string::npos);
}

TEST_CASE("a manifest that disagrees with its library is refused", "[plugin][loader]") {
    // The manifest is a claim; the binary is the truth. A manifest able to disagree with its own
    // library is a lie surface -- the user is shown one plugin's identity at install time while a
    // different plugin's code runs at load time, which makes the consent they gave meaningless.
    Host host;

    const auto wrongId = stagedCopy("wrong-id", "id = com.vcad.imposter\nabi_major = 1\n"
                                                "min_host_minor = 0\nlibrary = vcad_demo_plugin\n");
    const auto a = abi::loadPluginFrom(wrongId, host.host());
    REQUIRE_FALSE(a);
    CHECK(a.error().message.find("does not match its library") != std::string::npos);

    // Capabilities too: a plugin must not be able to request more than its manifest declared,
    // because the manifest is what the user was shown.
    const auto extraCaps = stagedCopy("caps", "id = com.vcad.demo\nabi_major = 1\n"
                                              "min_host_minor = 0\ncapabilities = network\n"
                                              "library = vcad_demo_plugin\n");
    const auto b = abi::loadPluginFrom(extraCaps, host.host());
    REQUIRE_FALSE(b);
    CHECK(b.error().message.find("more than its manifest") != std::string::npos);
}

TEST_CASE("a directory that is not a plugin is refused legibly", "[plugin][loader]") {
    Host host;

    const auto empty = std::filesystem::temp_directory_path() / "cad-plugin-test-empty";
    std::error_code ec;
    std::filesystem::create_directories(empty, ec);
    std::filesystem::remove(empty / "plugin.manifest", ec);
    const auto noManifest = abi::loadPluginFrom(empty, host.host());
    REQUIRE_FALSE(noManifest);
    CHECK(noManifest.error().message.find("no manifest") != std::string::npos);

    const auto missingLib = stagedCopy("missing-lib", "id = com.vcad.demo\nabi_major = 1\n"
                                                      "min_host_minor = 0\nlibrary = nothing\n");
    std::filesystem::remove(missingLib / (std::string("nothing") + abi::sharedLibrarySuffix()), ec);
    const auto gone = abi::loadPluginFrom(missingLib, host.host());
    REQUIRE_FALSE(gone);
    CHECK(gone.error().message.find("library is missing") != std::string::npos);
}

TEST_CASE("discovery finds plugin directories in a stable order", "[plugin][loader]") {
    const auto found = abi::discoverPluginDirectories(CAD_TEST_PLUGIN_DIR);
    REQUIRE_FALSE(found.empty());
    CHECK(std::is_sorted(found.begin(), found.end()));
    // Only directories carrying a manifest; the build tree has others beside them.
    for (const auto& dir : found) {
        CHECK(std::filesystem::exists(dir / "plugin.manifest"));
    }
}
