/// The naming scheme's version, stamped into every document.
///
/// # What it is for
///
/// Element names are written into saved files: a fillet stores "round the face called P1.0#4", not
/// "round that face". So changing how a name is DERIVED changes the meaning of every file already
/// written -- the fillet asks for a name nothing answers to, and a part finished last week opens
/// broken with nothing in the user's model to explain it.
///
/// An application has three options with such a file: refuse it, silently reinterpret it, or open
/// it and say what is uncertain. The middle one is what doing nothing gets you, and it is the one
/// that must never ship.
///
/// The stamp cannot be added retroactively -- a file written without it can never be told apart
/// from one written by any other build -- which is why it exists now, while vCAD is 0.0.1 and the
/// only documents in the world are our own.

#include "cad/app/Controller.h"
#include "cad/io/DocumentStore.h"
#include "cad/naming/ElementName.h"

#include <catch2/catch_test_macros.hpp>

#include <sqlite3.h>

#include <filesystem>
#include <string>

using namespace cad;

namespace {

std::filesystem::path scratch(const std::string& name) {
    const auto dir = std::filesystem::temp_directory_path() / "vcad_naming_scheme";
    std::filesystem::create_directories(dir);
    const auto path = dir / name;
    std::filesystem::remove(path);
    return path;
}

/// Rewrites the naming scheme version recorded in a saved file, to stand in for a document written
/// by a different build.
void setSchemeVersion(const std::filesystem::path& path, int version) {
    sqlite3* raw = nullptr;
    REQUIRE(sqlite3_open(path.string().c_str(), &raw) == SQLITE_OK);
    const std::string sql = "UPDATE meta SET value='" + std::to_string(version)
                            + "' WHERE key='naming_scheme_version'";
    REQUIRE(sqlite3_exec(raw, sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(raw);
}

document::Document savedBox() {
    app::Controller c;
    REQUIRE(c.beginCommand("feature.box"));
    REQUIRE(c.commitCommand());
    return c.document();
}

}   // namespace

TEST_CASE("every saved document records which naming scheme wrote it", "[naming][scheme]") {
    const auto path = scratch("stamped.vpart");
    REQUIRE(io::saveDocument(savedBox(), path));

    const auto info = io::readDocumentInfo(path);
    REQUIRE(info);
    CHECK(info.value().namingSchemeVersion == naming::kNamingSchemeVersion);
}

TEST_CASE("a document from an older scheme still opens", "[naming][scheme]") {
    // It opens because the user's work is in it. Some of its references may no longer resolve, and
    // that is worth saying -- but refusing the file would mean they cannot reach the parts that are
    // still fine, which is a worse trade than a warning.
    const auto path = scratch("older.vpart");
    REQUIRE(io::saveDocument(savedBox(), path));
    setSchemeVersion(path, naming::kNamingSchemeVersion - 1);

    const auto info = io::readDocumentInfo(path);
    REQUIRE(info);
    CHECK(info.value().namingSchemeVersion < naming::kNamingSchemeVersion);

    app::Controller c;
    const auto opened = c.loadFrom(path);
    if (!opened) INFO(opened.error().message);
    CHECK(opened.ok());
}

TEST_CASE("opening an older document says so", "[naming][scheme]") {
    // The whole point. A part that opens with red features and no explanation is the failure this
    // stamp exists to prevent; the message is what turns it into something the user can act on.
    const auto path = scratch("older_reported.vpart");
    REQUIRE(io::saveDocument(savedBox(), path));
    setSchemeVersion(path, naming::kNamingSchemeVersion - 1);

    app::Controller c;
    std::string said;
    c.onStatus([&](const std::string& text) { said = text; });
    REQUIRE(c.loadFrom(path).ok());
    INFO(said);
    CHECK(said.find("older naming scheme") != std::string::npos);
}

TEST_CASE("a document from a NEWER scheme is refused", "[naming][scheme]") {
    // The opposite trade. Its references were derived by rules this build does not have, so they
    // would either fail to resolve or -- far worse -- resolve to whatever this build happens to
    // name the same way. Opening it would be the silent reinterpretation, on geometry the user
    // cannot check.
    const auto path = scratch("newer.vpart");
    REQUIRE(io::saveDocument(savedBox(), path));
    setSchemeVersion(path, naming::kNamingSchemeVersion + 1);

    const auto loaded = io::loadDocument(path);
    REQUIRE_FALSE(loaded.ok());
    INFO(loaded.error().message);
    CHECK(loaded.error().message.find("newer version") != std::string::npos);
}

TEST_CASE("a document written before the stamp existed reads as unknown", "[naming][scheme]") {
    // Version 0, which is what an unstamped file looks like, and exactly the situation the stamp
    // exists to stop recurring: nothing can be said about how its names were derived.
    const auto path = scratch("unstamped.vpart");
    REQUIRE(io::saveDocument(savedBox(), path));

    sqlite3* raw = nullptr;
    REQUIRE(sqlite3_open(path.string().c_str(), &raw) == SQLITE_OK);
    REQUIRE(sqlite3_exec(raw, "DELETE FROM meta WHERE key='naming_scheme_version'", nullptr,
                         nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(raw);

    const auto info = io::readDocumentInfo(path);
    REQUIRE(info);
    CHECK(info.value().namingSchemeVersion == 0);

    // And it still opens, reported as older -- which it is.
    app::Controller c;
    CHECK(c.loadFrom(path).ok());
}
