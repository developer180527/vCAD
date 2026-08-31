/// Named parameters: `width = 40mm`, `wall = width / 8`.
///
/// # The two failures worth writing tests for
///
/// **A cycle must not be a crash.** `a = b + 1` with `b = a + 1` is a mistake a user makes by
/// typing two perfectly reasonable things in the wrong order. An evaluator that simply asks for
/// the value it needs recurses until the stack runs out, and the application dies with no message
/// and no saved work. Resolution is therefore iterative over a graph built in advance, and these
/// tests include the loops that would kill a naive one.
///
/// **Geometry must not depend on who opened the file.** Display units are a user preference, so
/// two engineers opening the same document have different ones. Every number that reaches the
/// kernel has to be identical for both of them, and the last test here asserts that end to end --
/// same file, two preferences, byte-identical document digest.

#include "cad/document/Parameters.h"
#include "cad/io/DocumentStore.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

using namespace cad;
using Catch::Approx;
using document::ObjectData;
using document::ObjectId;
using document::Property;
using units::UnitSystem;

namespace {

Property lengthParameter(std::string name, double millimetres) {
    return Property{std::move(name), units::Length::fromBase(millimetres), false, {},
                    UnitSystem::Millimetre};
}

Property derived(std::string name, std::string expression,
                 UnitSystem enteredIn = UnitSystem::Millimetre) {
    // The stored value starts at zero and is filled in by resolution; declaring it a Length is what
    // says "this parameter is a length", which is what the expression is checked against.
    return Property{std::move(name), units::Length::fromBase(0.0), false, std::move(expression),
                    enteredIn};
}

double valueOf(const document::ParameterValues& values, const std::string& name) {
    const auto found = values.values.find(name);
    REQUIRE(found != values.values.end());
    return found->second.magnitude;
}

std::string problemFor(const document::ParameterValues& values, const std::string& name) {
    for (const auto& problem : values.problems) {
        if (problem.name == name) return problem.message;
    }
    return {};
}

std::filesystem::path scratch(const std::string& name) {
    const auto dir = std::filesystem::temp_directory_path() / "vcad_parameters";
    std::filesystem::create_directories(dir);
    return dir / name;
}

}   // namespace

TEST_CASE("a parameter resolves to its value", "[parameters]") {
    const auto doc = document::Document{}.withParameter(lengthParameter("width", 40.0));
    const auto values = document::resolveParameters(doc);
    CHECK(values.problems.empty());
    CHECK(valueOf(values, "width") == Approx(40.0));
}

TEST_CASE("a parameter can be defined in terms of another", "[parameters]") {
    auto doc = document::Document{}
                   .withParameter(lengthParameter("width", 40.0))
                   .withParameter(derived("wall", "width / 8"))
                   .withParameter(derived("flange", "wall * 3 + 1mm"));

    const auto values = document::resolveParameters(doc);
    INFO("first problem: " << problemFor(values, "wall"));
    CHECK(values.problems.empty());
    CHECK(valueOf(values, "wall") == Approx(5.0));
    CHECK(valueOf(values, "flange") == Approx(16.0));
}

TEST_CASE("resolution order does not depend on the order they were created", "[parameters]") {
    // Parameters are stored sorted by name, so `wall` sorts before `width` -- the dependency is
    // resolved second in list order and must still be seen first.
    const auto doc = document::Document{}
                         .withParameter(derived("wall", "width / 8"))
                         .withParameter(lengthParameter("width", 40.0));
    const auto values = document::resolveParameters(doc);
    CHECK(values.problems.empty());
    CHECK(valueOf(values, "wall") == Approx(5.0));
}

TEST_CASE("a cycle is reported, not followed", "[parameters][cycles]") {
    // If this test ever fails it will not fail -- it will hang or crash the runner, which is the
    // whole reason resolution is iterative.
    const auto doc = document::Document{}
                         .withParameter(derived("a", "b + 1mm"))
                         .withParameter(derived("b", "a + 1mm"));

    const auto values = document::resolveParameters(doc);
    REQUIRE(values.problems.size() == 2);
    const auto message = problemFor(values, "a");
    INFO(message);
    // Both names, so the user can see what to change. "a is invalid" is not actionable when the
    // list has forty entries.
    CHECK(message.find("a") != std::string::npos);
    CHECK(message.find("b") != std::string::npos);
    CHECK(values.values.empty());
}

TEST_CASE("a longer cycle is reported with every parameter in it", "[parameters][cycles]") {
    const auto doc = document::Document{}
                         .withParameter(derived("a", "c + 1mm"))
                         .withParameter(derived("b", "a + 1mm"))
                         .withParameter(derived("c", "b + 1mm"));

    const auto values = document::resolveParameters(doc);
    CHECK(values.problems.size() == 3);
    const auto message = problemFor(values, "a");
    INFO(message);
    CHECK(message.find("a") != std::string::npos);
    CHECK(message.find("b") != std::string::npos);
    CHECK(message.find("c") != std::string::npos);
}

TEST_CASE("a parameter defined in terms of itself says so plainly", "[parameters][cycles]") {
    const auto doc = document::Document{}.withParameter(derived("width", "width * 2"));
    const auto values = document::resolveParameters(doc);
    REQUIRE(values.problems.size() == 1);
    INFO(values.problems.front().message);
    CHECK(values.problems.front().message.find("itself") != std::string::npos);
}

TEST_CASE("one bad parameter does not blank out the others", "[parameters]") {
    // A list of forty parameters where one has a typo must still drive the other thirty-nine. The
    // alternative is a model that collapses to nothing while the user is mid-edit.
    const auto doc = document::Document{}
                         .withParameter(lengthParameter("width", 40.0))
                         .withParameter(derived("broken", "widht * 2"))
                         .withParameter(derived("wall", "width / 8"));

    const auto values = document::resolveParameters(doc);
    REQUIRE(values.problems.size() == 1);
    CHECK(values.problems.front().name == "broken");
    CHECK(valueOf(values, "width") == Approx(40.0));
    CHECK(valueOf(values, "wall") == Approx(5.0));
}

TEST_CASE("a parameter downstream of a broken one is reported too", "[parameters]") {
    const auto doc = document::Document{}
                         .withParameter(derived("broken", "nosuch * 2"))
                         .withParameter(derived("uses_it", "broken + 1mm"));
    const auto values = document::resolveParameters(doc);
    CHECK(values.problems.size() == 2);
    CHECK(values.values.empty());
}

TEST_CASE("a parameter keeps the kind it was declared with", "[parameters][units]") {
    // `wall = width * width` in a length parameter is an area. Letting it through would hand an
    // area to every feature that uses `wall`.
    const auto doc = document::Document{}
                         .withParameter(lengthParameter("width", 40.0))
                         .withParameter(derived("wall", "width * width"));
    const auto values = document::resolveParameters(doc);
    REQUIRE(values.problems.size() == 1);
    INFO(values.problems.front().message);
    CHECK(values.problems.front().message.find("mm^2") != std::string::npos);
}

TEST_CASE("changing a parameter rebuilds every property that uses it", "[parameters]") {
    auto doc = document::Document{}.withParameter(lengthParameter("width", 40.0));
    auto [added, id] = doc.add("Extrude");
    auto object = added.find(id)->withExpression("depth", units::Length::fromBase(0.0), "width * 2",
                                                 UnitSystem::Millimetre);
    doc = added.replace(std::make_shared<const ObjectData>(std::move(object)));

    auto first = document::rebuildFromParameters(doc);
    CHECK(first.problems.empty());
    CHECK(std::get<units::Length>(*first.document.find(id)->find("depth")).base() == Approx(80.0));

    const auto edited = first.document.withParameter(lengthParameter("width", 60.0));
    const auto second = document::rebuildFromParameters(edited);
    CHECK(std::get<units::Length>(*second.document.find(id)->find("depth")).base() == Approx(120.0));
}

TEST_CASE("a derived parameter's stored value is the resolved one", "[parameters][storage]") {
    // Otherwise the file claims `wall = 0mm` while every feature using wall was built from 5, and
    // anything reading the document without an evaluator believes it.
    const auto doc = document::Document{}
                         .withParameter(lengthParameter("width", 40.0))
                         .withParameter(derived("wall", "width / 8"));
    CHECK(std::get<units::Length>(doc.parameter("wall")->value).base() == Approx(0.0));

    const auto rebuilt = document::rebuildFromParameters(doc).document;
    CHECK(std::get<units::Length>(rebuilt.parameter("wall")->value).base() == Approx(5.0));
    CHECK(rebuilt.parameter("wall")->expression == "width / 8");   // still derived, not flattened

    const auto path = scratch("resolved.vpart");
    REQUIRE(io::saveDocument(rebuilt, path));
    const auto loaded = io::loadDocument(path);
    REQUIRE(loaded);
    CHECK(std::get<units::Length>(loaded.value().parameter("wall")->value).base() == Approx(5.0));
}

TEST_CASE("parameters survive a save and a load", "[parameters][storage]") {
    const auto doc = document::Document{}
                         .withParameter(lengthParameter("width", 40.0))
                         .withParameter(derived("wall", "width / 8", UnitSystem::Inch));
    const auto path = scratch("params.vpart");
    REQUIRE(io::saveDocument(doc, path));

    const auto loaded = io::loadDocument(path);
    REQUIRE(loaded);
    REQUIRE(loaded.value().parameters().size() == 2);
    const auto* wall = loaded.value().parameter("wall");
    REQUIRE(wall != nullptr);
    CHECK(wall->expression == "width / 8");
    CHECK(wall->expressionUnits == UnitSystem::Inch);
    CHECK(document::resolveParameters(loaded.value()).values.size() == 2);
}

TEST_CASE("a name must be usable in an expression", "[parameters]") {
    const auto doc = document::Document{}.withParameter(lengthParameter("width", 40.0));
    CHECK(document::parameterNameProblem(doc, "height").empty());
    CHECK_FALSE(document::parameterNameProblem(doc, "width").empty());       // duplicate
    CHECK_FALSE(document::parameterNameProblem(doc, "2wide").empty());       // not an identifier
    CHECK_FALSE(document::parameterNameProblem(doc, "wall thickness").empty());
    CHECK_FALSE(document::parameterNameProblem(doc, "pi").empty());          // built-in
    CHECK_FALSE(document::parameterNameProblem(doc, "in").empty());          // a unit
    CHECK_FALSE(document::parameterNameProblem(doc, "").empty());
}

TEST_CASE("editing a parameter counts as an unsaved change", "[parameters]") {
    const auto before = document::Document{}.withParameter(lengthParameter("width", 40.0));
    const auto renamedValue = before.withParameter(lengthParameter("width", 41.0));
    CHECK(before.digest() != renamedValue.digest());

    // Adding one nothing uses yet is still an edit to the document.
    const auto extra = before.withParameter(lengthParameter("unused", 1.0));
    CHECK(before.digest() != extra.digest());
}

TEST_CASE("the same document gives the same geometry to two users with different units",
          "[parameters][units][determinism]") {
    // The invariant, end to end. Two engineers open one file. Their display-unit preferences
    // differ. Every number that reaches the kernel must be identical -- if it is not, one of them
    // machines a part that is 25.4 times the size of the other's, and neither has any way to tell.
    //
    // `wall = width / 8` and a depth of `wall + 10` both contain bare numbers, which are exactly
    // the values a preference could leak into.
    //
    // `wall` is deliberately entered in INCHES with a bare number in it, so that a resolver which
    // ignored the entry unit -- or substituted the reader's preference -- would produce 5 + 1 = 6mm
    // for one user and 5 + 25.4 = 30.4mm for the other.
    auto doc = document::Document{}
                   .withParameter(lengthParameter("width", 40.0))
                   .withParameter(derived("wall", "width / 8 + 1", UnitSystem::Inch));
    auto [added, id] = doc.add("Extrude");
    auto object = added.find(id)->withExpression("depth", units::Length::fromBase(0.0), "wall + 10",
                                                 UnitSystem::Millimetre);
    doc = added.replace(std::make_shared<const ObjectData>(std::move(object)));

    const auto path = scratch("shared.vpart");
    REQUIRE(io::saveDocument(document::rebuildFromParameters(doc).document, path));

    // Two independent opens. Nothing about a user's preference is passed in -- and that is the
    // point: there is no argument to pass. Resolution takes the document and nothing else.
    const auto first = io::loadDocument(path);
    const auto second = io::loadDocument(path);
    REQUIRE(first);
    REQUIRE(second);

    const auto rebuiltByFirst = document::rebuildFromParameters(first.value());
    const auto rebuiltBySecond = document::rebuildFromParameters(second.value());

    CHECK(rebuiltByFirst.problems.empty());
    CHECK(rebuiltBySecond.problems.empty());
    CHECK(rebuiltByFirst.document.digest() == rebuiltBySecond.document.digest());

    // And the actual numbers are the ones that were typed: wall = 40/8 mm + 1 INCH, and a depth of
    // wall + 10 MILLIMETRES.
    const auto resolved = document::resolveParameters(rebuiltByFirst.document);
    CHECK(valueOf(resolved, "wall") == Approx(5.0 + 25.4));
    CHECK(std::get<units::Length>(*rebuiltByFirst.document.find(id)->find("depth")).base()
          == Approx(5.0 + 25.4 + 10.0));
}
