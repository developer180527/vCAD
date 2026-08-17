/// The one way a plugin enters a process: `cad_plugins_load`.
///
/// This is the seam the shell sits on. Everything else in the plugin thread — ribbon tabs, settings
/// pages, feature types — is tested by registering against a host vtable directly, which proves the
/// registration works but proves nothing about a real shared library being found on disk, opened,
/// verified and initialised. This file covers exactly that gap, because it is the part that was
/// missing from the application: the ABI could carry a plugin's settings page long before anything
/// in the shell had ever loaded a plugin.
///
/// The load must survive its own failure modes without help. A directory that does not exist is the
/// common case on a fresh machine and must not be an error; a plugin that refuses to load must not
/// take the others with it. Both are asserted here rather than left to the shell, because a shell
/// that has to defend against its own loader would push that duty onto the second shell too.

#include "cad/abi/cad_plugin_abi.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>

namespace {

/// A session with nothing loaded, freed by the test that made it.
struct OwnedSession {
    CadSession handle = 0;
    OwnedSession() { handle = cad_session_create(); }
    ~OwnedSession() { if (handle != 0) cad_session_release(handle); }
    OwnedSession(const OwnedSession&) = delete;
    OwnedSession& operator=(const OwnedSession&) = delete;
};

}  // namespace

TEST_CASE("a real plugin directory loads through the ABI entry point", "[plugin][abi]") {
    OwnedSession session;
    REQUIRE(session.handle != 0);

    std::uint32_t loaded = 99;
    std::uint32_t failed = 99;
    REQUIRE(cad_plugins_load(session.handle, CAD_TEST_PLUGIN_DIR, &loaded, &failed) == CAD_OK);

    // The demo plugin is built as a dependency of this suite, so exactly one is expected. Asserted
    // as a NUMBER rather than ">= 0": a load that silently found nothing is the failure this test
    // exists to catch, and it is indistinguishable from success without this line.
    CHECK(loaded == 1);
    CHECK(failed == 0);
}

TEST_CASE("a loaded plugin's feature type is usable afterwards", "[plugin][abi]") {
    OwnedSession session;
    std::uint32_t loaded = 0;
    REQUIRE(cad_plugins_load(session.handle, CAD_TEST_PLUGIN_DIR, &loaded, nullptr) == CAD_OK);
    REQUIRE(loaded == 1);

    // The point of loading at all. Loading that leaves nothing usable is indistinguishable from
    // not loading, which is how this looked from the application before the entry point existed.
    // The demo plugin registers a feature and no ribbon items, so the feature is what to ask for —
    // an assertion about ribbon commands here would be a claim about the fixture, not the loader.
    CadObject object = 0;
    CHECK(cad_object_add(session.handle, "com.vcad.demo.Cube", &object) == CAD_OK);

    // And an unregistered type must still be refused, so the check above is not passing because
    // every string is accepted.
    CadObject bogus = 0;
    CHECK(cad_object_add(session.handle, "com.vcad.demo.NotAThing", &bogus) != CAD_OK);
}

TEST_CASE("a directory with no plugins is not an error", "[plugin][abi]") {
    OwnedSession session;

    const std::filesystem::path empty =
        std::filesystem::temp_directory_path() / "vcad_no_plugins_here";
    std::filesystem::create_directories(empty);

    std::uint32_t loaded = 7;
    std::uint32_t failed = 7;
    CHECK(cad_plugins_load(session.handle, empty.string().c_str(), &loaded, &failed) == CAD_OK);
    CHECK(loaded == 0);
    CHECK(failed == 0);

    // A machine that has never installed a plugin must be indistinguishable from a healthy one. If
    // this returned an error the shell would show a plugin failure to every new user.
    std::uint32_t loaded2 = 7;
    CHECK(cad_plugins_load(session.handle, (empty / "gone").string().c_str(), &loaded2, nullptr)
          == CAD_OK);
    CHECK(loaded2 == 0);

    std::filesystem::remove_all(empty);
}

TEST_CASE("one bad plugin does not stop the good one", "[plugin][abi]") {
    // A directory holding BOTH the working demo plugin and a broken entry. The whole point of
    // counting failures rather than returning on the first one is that a user with two plugins,
    // one of which is stale after an update, still gets the other.
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "vcad_mixed_plugins";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    std::error_code ec;
    std::filesystem::copy(std::filesystem::path(CAD_TEST_PLUGIN_DIR) / "demo", root / "demo",
                          std::filesystem::copy_options::recursive, ec);
    REQUIRE_FALSE(ec);

    // Sorted before "demo", so the failure happens FIRST. A loop that returned early on failure
    // would pass this test if the broken plugin sorted last.
    std::filesystem::create_directories(root / "broken");
    {
        std::ofstream manifest(root / "broken" / "plugin.manifest");
        manifest << "this is not a manifest\n";
    }

    OwnedSession session;
    std::uint32_t loaded = 0;
    std::uint32_t failed = 0;
    REQUIRE(cad_plugins_load(session.handle, root.string().c_str(), &loaded, &failed) == CAD_OK);

    CHECK(loaded == 1);
    CHECK(failed == 1);

    // And the user can be told which one. A count with no reason is not something a shell can put
    // in front of a person.
    const char* message = cad_session_last_error(session.handle);
    REQUIRE(message != nullptr);
    CHECK(std::string(message).size() > 0);

    std::filesystem::remove_all(root);
}
