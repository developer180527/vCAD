/// Exporting writes the visible bodies, and writes them so they can be read back.
///
/// # Why this reads the file back
///
/// "The writer returned success" is the assertion this whole area did not need — `core/io` has had
/// tested STEP, IGES and STL writers for weeks, and the audit still called export the cheapest
/// disqualifying gap, because nothing in the application ever called them. So the interesting
/// question is not whether the writer works. It is whether what the APPLICATION chose to hand it
/// is the right geometry.
///
/// Reading the file back with the importer answers that: the round trip goes through two
/// independent implementations of the format, and volume survives it.

#include "cad/app/Controller.h"
#include "cad/io/Format.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <filesystem>

using namespace cad;

namespace {

std::filesystem::path scratch(const std::string& name) {
    return std::filesystem::temp_directory_path() / name;
}

/// Invokes a ribbon command. beginCommand only accepts commands that take PARAMETERS and returns
/// false for the rest, so a command like Fillet has to be run through its own invoke.
bool run(app::Controller& c, const std::string& id) {
    for (const auto& command : c.commands()) {
        if (command.id == id) {
            if (command.invoke) command.invoke();
            return true;
        }
    }
    return false;
}

document::ObjectId firstOfType(app::Controller& c, const std::string& type) {
    for (const auto& item : c.tree()) {
        if (item.type == type) return item.id;
    }
    return {};
}

}  // namespace

TEST_CASE("a box exports and comes back the same size", "[export]") {
    app::Controller c;
    REQUIRE(c.beginCommand("feature.box"));
    REQUIRE(c.commitCommand());

    const document::ObjectId boxId = firstOfType(c, "Box");
    REQUIRE(boxId != document::ObjectId{});
    const double expected = c.document().find(boxId)->output()->shape.volume();
    REQUIRE(expected > 0.0);

    const auto path = scratch("vcad_export_box.step");
    std::filesystem::remove(path);
    REQUIRE(c.exportDocument(path.string()));
    REQUIRE(std::filesystem::exists(path));
    CHECK(std::filesystem::file_size(path) > 0);

    // Read back through the importer — a different code path from the writer, so agreement means
    // the file is right rather than that one implementation is self-consistent.
    const auto registry = io::FormatRegistry::builtins();
    auto imported = io::importFile(registry, path.string());
    REQUIRE(imported);
    CHECK_THAT(imported.value().shape.volume(), Catch::Matchers::WithinRel(expected, 0.001));

    std::filesystem::remove(path);
}

TEST_CASE("export writes what is on screen, not what is in the document", "[export]") {
    // The decision this feature actually had to make. A Box consumed by a Fillet is still IN the
    // document; writing both would put the un-filleted block in the file beside the real part, and
    // the user would discover it in whatever opened the file.
    app::Controller c;
    REQUIRE(c.beginCommand("feature.box"));
    REQUIRE(c.commitCommand());

    const document::ObjectId boxId = firstOfType(c, "Box");
    REQUIRE(boxId != document::ObjectId{});
    c.select(boxId, false);
    REQUIRE(run(c, "feature.fillet"));

    const document::ObjectId filletId = firstOfType(c, "Fillet");
    REQUIRE(filletId != document::ObjectId{});
    const double filleted = c.document().find(filletId)->output()->shape.volume();
    const double raw = c.document().find(boxId)->output()->shape.volume();
    // The fillet removed material, so the two volumes differ — which is what makes this test able
    // to tell which one was written.
    REQUIRE(filleted < raw);

    const auto path = scratch("vcad_export_tip.step");
    std::filesystem::remove(path);
    REQUIRE(c.exportDocument(path.string()));

    const auto registry = io::FormatRegistry::builtins();
    auto imported = io::importFile(registry, path.string());
    REQUIRE(imported);
    CHECK_THAT(imported.value().shape.volume(), Catch::Matchers::WithinRel(filleted, 0.001));

    std::filesystem::remove(path);
}

TEST_CASE("export refuses rather than writing something useless", "[export]") {
    app::Controller c;

    // An empty document. A zero-byte STEP file that opens as nothing is worse than being told
    // there is nothing to export.
    const auto empty = scratch("vcad_export_empty.step");
    std::filesystem::remove(empty);
    CHECK_FALSE(c.exportDocument(empty.string()));
    CHECK_FALSE(std::filesystem::exists(empty));

    REQUIRE(c.beginCommand("feature.box"));
    REQUIRE(c.commitCommand());

    // An extension nothing handles, refused BEFORE any file is created.
    const auto bogus = scratch("vcad_export.wat");
    std::filesystem::remove(bogus);
    CHECK_FALSE(c.exportDocument(bogus.string()));
    CHECK_FALSE(std::filesystem::exists(bogus));

    CHECK_FALSE(c.exportDocument(""));
}

TEST_CASE("the format list is what the build can actually write", "[export]") {
    const auto formats = app::Controller::exportFormats();
    REQUIRE_FALSE(formats.empty());

    const auto registry = io::FormatRegistry::builtins();
    for (const auto& format : formats) {
        REQUIRE_FALSE(format.extensions.empty());
        // Every offered format must resolve for its own extension, or the dialog is promising a
        // format that will be refused the moment the user picks it.
        CHECK(registry.forPath("x" + format.extensions.front()) != nullptr);
        CHECK_FALSE(format.displayName.empty());
    }

    // STL is mesh-only and must say so; the dialog uses this to warn before the B-rep is lost.
    for (const auto& format : formats) {
        if (format.id == "stl") CHECK_FALSE(format.solids);
    }
}
