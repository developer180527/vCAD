/// Storing the formula, not just the answer.
///
/// # The failure this exists to prevent
///
/// A property that keeps only the 80 that `width * 2` evaluated to LOOKS completely correct. The
/// model rebuilds, the part is the right size, every test that checks geometry passes. What is
/// gone is invisible until the user needs it: changing `width` moves nothing, and reopening the
/// file shows a plain number where a relationship used to be. By then the relationship cannot be
/// recovered -- nobody knows which of the numbers in the model were once formulas.
///
/// So these tests are almost all about PERSISTENCE and PROPAGATION rather than arithmetic:
///
///   * the text survives a save and a load,
///   * a document written before expressions existed still opens,
///   * a bare number inside a stored expression keeps meaning what it meant when it was typed,
///     even for a colleague whose units are different,
///   * setting a plain value breaks the link, loudly and on purpose,
///   * "this file has unsaved changes" notices a formula edit that leaves the number identical.

#include "cad/document/Document.h"
#include "cad/document/Expressions.h"
#include "cad/io/DocumentStore.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <sqlite3.h>

#include <filesystem>
#include <map>
#include <string>

using namespace cad;
using Catch::Approx;
using document::ObjectData;
using document::ObjectId;
using document::PropertyValue;
using units::UnitSystem;

namespace {

expr::Resolver table(std::map<std::string, double> lengthsInMillimetres) {
    return [values = std::move(lengthsInMillimetres)](
               std::string_view name) -> std::optional<expr::Value> {
        const auto found = values.find(std::string(name));
        if (found == values.end()) return std::nullopt;
        return expr::Value{found->second, expr::kLength, false};
    };
}

/// A document with one feature whose `depth` is driven by an expression.
std::pair<document::Document, ObjectId> withDrivenDepth(const std::string& expression,
                                                        UnitSystem enteredIn,
                                                        double initialMillimetres = 0.0) {
    document::Document doc;
    auto [next, id] = doc.add("Extrude");
    auto object = *next.find(id);
    object = object.withExpression("depth", units::Length::fromBase(initialMillimetres), expression,
                                   enteredIn);
    return {next.replace(std::make_shared<const ObjectData>(std::move(object))), id};
}

double depthOf(const document::Document& doc, ObjectId id) {
    const auto object = doc.find(id);
    REQUIRE(object);
    const auto* value = object->find("depth");
    REQUIRE(value != nullptr);
    return std::get<units::Length>(*value).base();
}

std::filesystem::path scratch(const std::string& name) {
    const auto dir = std::filesystem::temp_directory_path() / "vcad_expression_storage";
    std::filesystem::create_directories(dir);
    return dir / name;
}

}   // namespace

TEST_CASE("a property remembers the expression that produced it", "[expressions][storage]") {
    auto [doc, id] = withDrivenDepth("width * 2", UnitSystem::Millimetre);
    const auto* property = doc.find(id)->property("depth");
    REQUIRE(property != nullptr);
    CHECK(property->expression == "width * 2");

    // And a plain property has none, which is what keeps the common case free of ceremony.
    auto plain = doc.find(id)->withProperty("angle", units::Angle::fromBase(1.0));
    CHECK(plain.property("angle")->expression.empty());
}

TEST_CASE("re-evaluating an expression updates the value", "[expressions][storage]") {
    auto [doc, id] = withDrivenDepth("width * 2", UnitSystem::Millimetre);

    auto first = document::reevaluate(doc, table({{"width", 40.0}}));
    CHECK(first.problems.empty());
    CHECK(first.changed == 1);
    CHECK(depthOf(first.document, id) == Approx(80.0));

    // The point of the whole feature: change the parameter, the model follows.
    auto second = document::reevaluate(first.document, table({{"width", 60.0}}));
    CHECK(second.changed == 1);
    CHECK(depthOf(second.document, id) == Approx(120.0));

    // And the expression is still there afterwards -- evaluating must not consume it.
    CHECK(second.document.find(id)->property("depth")->expression == "width * 2");

    // Re-evaluating with the same inputs changes nothing, so nothing is needlessly dirtied.
    auto again = document::reevaluate(second.document, table({{"width", 60.0}}));
    CHECK(again.changed == 0);
}

TEST_CASE("an expression survives a save and a load", "[expressions][storage]") {
    auto [doc, id] = withDrivenDepth("width * 2 + 1mm", UnitSystem::Inch, 81.0);
    const auto path = scratch("driven.vpart");

    REQUIRE(io::saveDocument(doc, path));
    const auto loaded = io::loadDocument(path);
    REQUIRE(loaded);

    const auto* property = loaded.value().find(id)->property("depth");
    REQUIRE(property != nullptr);
    CHECK(property->expression == "width * 2 + 1mm");
    // The unit the text was typed in travels with it -- see the next test for why that matters.
    CHECK(property->expressionUnits == UnitSystem::Inch);
    CHECK(std::get<units::Length>(*loaded.value().find(id)->find("depth")).base() == Approx(81.0));
}

TEST_CASE("a bare number in a stored expression keeps the unit it was typed in",
          "[expressions][storage][units]") {
    // The bug this prevents is the worst one this format could have: geometry that depends on WHO
    // OPENED THE FILE. Display units are a user preference, not a document property, so an
    // expression of `width + 10` re-evaluated under a colleague's inch preference would become
    // `width + 254mm` and quietly resize their part.
    auto [doc, id] = withDrivenDepth("width + 10", UnitSystem::Millimetre);

    const auto path = scratch("units.vpart");
    REQUIRE(io::saveDocument(doc, path));
    const auto loaded = io::loadDocument(path);
    REQUIRE(loaded);

    const auto result = document::reevaluate(loaded.value(), table({{"width", 40.0}}));
    CHECK(result.problems.empty());
    // 50mm, as typed. Not 40 + 254.
    CHECK(depthOf(result.document, id) == Approx(50.0));

    // The same text entered in an inch document means something different, and keeps meaning it.
    auto [inches, inchId] = withDrivenDepth("width + 10", UnitSystem::Inch);
    const auto inchResult = document::reevaluate(inches, table({{"width", 40.0}}));
    CHECK(depthOf(inchResult.document, inchId) == Approx(40.0 + 254.0));
}

TEST_CASE("setting a plain value breaks the link", "[expressions][storage]") {
    // Typing a number over a formula, or dragging a handle that writes the property directly,
    // removes the formula. Keeping it would leave the stored text and the stored number
    // disagreeing, and the next rebuild would silently undo the user's edit.
    auto [doc, id] = withDrivenDepth("width * 2", UnitSystem::Millimetre);
    auto object = doc.find(id)->withProperty("depth", units::Length::fromBase(12.0));
    CHECK(object.property("depth")->expression.empty());

    auto after = document::reevaluate(
        doc.replace(std::make_shared<const ObjectData>(std::move(object))),
        table({{"width", 40.0}}));
    CHECK(after.changed == 0);
    CHECK(depthOf(after.document, id) == Approx(12.0));
}

TEST_CASE("a broken expression keeps the last good value and reports the feature",
          "[expressions][storage]") {
    // Deleting a parameter something depends on must not rebuild the part at zero. A part that
    // silently becomes 0mm deep still exports, still renders, and is scrap.
    auto [doc, id] = withDrivenDepth("width * 2", UnitSystem::Millimetre, 80.0);

    const auto result = document::reevaluate(doc, table({{"height", 10.0}}));
    REQUIRE(result.problems.size() == 1);
    CHECK(result.problems.front().object == id);
    CHECK(result.problems.front().property == "depth");
    CHECK(result.problems.front().message.find("width") != std::string::npos);

    CHECK(depthOf(result.document, id) == Approx(80.0));            // last good value, untouched
    CHECK(result.document.find(id)->state() == document::ObjectState::Failed);
    CHECK(result.document.find(id)->error().message.find("depth") != std::string::npos);
}

TEST_CASE("an expression must produce the kind of value the property holds",
          "[expressions][storage][units]") {
    // A depth is a length whatever the user typed into it. `width * width` is an area, and a field
    // that accepted it would make a 40mm input into a 1600mm part.
    auto [doc, id] = withDrivenDepth("width * width", UnitSystem::Millimetre, 5.0);
    const auto result = document::reevaluate(doc, table({{"width", 40.0}}));

    REQUIRE(result.problems.size() == 1);
    INFO(result.problems.front().message);
    CHECK(result.problems.front().message.find("mm^2") != std::string::npos);
    CHECK(depthOf(result.document, id) == Approx(5.0));
}

TEST_CASE("editing a formula counts as an unsaved change", "[expressions][storage]") {
    // Document::digest() is what the application asks before closing without saving. Replacing 80
    // with `width * 2` leaves the number identical and the model profoundly different; a digest
    // blind to the text would let that edit be thrown away with no prompt.
    document::Document plain;
    auto [added, id] = plain.add("Extrude");
    auto object = added.find(id)->withProperty("depth", units::Length::fromBase(80.0));
    const auto before =
        added.replace(std::make_shared<const ObjectData>(std::move(object)));

    auto driven = before.find(id)->withExpression("depth", units::Length::fromBase(80.0),
                                                  "width * 2", UnitSystem::Millimetre);
    const auto after = before.replace(std::make_shared<const ObjectData>(std::move(driven)));

    CHECK(before.digest() != after.digest());
}

TEST_CASE("a document written before expressions existed still opens",
          "[expressions][storage][compatibility]") {
    // Written by hand in the version 1 schema -- no expression columns at all. The loader asks the
    // FILE which columns it has rather than trusting the version number, so this is the real
    // compatibility path and not a simulation of it.
    const auto path = scratch("legacy_v1.vpart");
    std::filesystem::remove(path);

    {
        sqlite3* raw = nullptr;
        REQUIRE(sqlite3_open(path.string().c_str(), &raw) == SQLITE_OK);
        const char* legacy = R"sql(
            CREATE TABLE meta (key TEXT PRIMARY KEY, value TEXT NOT NULL) STRICT;
            CREATE TABLE objects (id INTEGER PRIMARY KEY, type TEXT NOT NULL, label TEXT NOT NULL)
              STRICT;
            CREATE TABLE properties (
              object_id INTEGER NOT NULL REFERENCES objects(id) ON DELETE CASCADE,
              name TEXT NOT NULL, tag INTEGER NOT NULL,
              cosmetic INTEGER NOT NULL DEFAULT 0, value TEXT NOT NULL,
              PRIMARY KEY (object_id, name)) STRICT;
            INSERT INTO meta VALUES ('schema_version','1'), ('kind','Part'),
                                    ('application','vCAD test'), ('next_object_id','2');
            INSERT INTO objects VALUES (1, 'Extrude', 'Extrude1');
            INSERT INTO properties (object_id, name, tag, cosmetic, value)
              VALUES (1, 'depth', 5, 0, '25');
        )sql";
        char* message = nullptr;
        const int rc = sqlite3_exec(raw, legacy, nullptr, nullptr, &message);
        if (message != nullptr) sqlite3_free(message);
        sqlite3_close(raw);
        REQUIRE(rc == SQLITE_OK);
    }

    const auto loaded = io::loadDocument(path);
    REQUIRE(loaded);
    const auto object = loaded.value().find(ObjectId{1});
    REQUIRE(object);
    CHECK(std::get<units::Length>(*object->find("depth")).base() == Approx(25.0));
    CHECK(object->property("depth")->expression.empty());
}
