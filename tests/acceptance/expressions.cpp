/// Expressions: what a user may type where a number is expected.
///
/// # What these tests are defending
///
/// An expression evaluator is easy to write and easy to write WRONG in ways that never throw. The
/// three failures that matter here all produce a plausible number:
///
///   1. `width * 2` treating the 2 as 2mm, or `width + 10` treating the 10 as a pure scalar. One
///      rule cannot be right for both, so the code has two, and swapping them silently doubles or
///      halves a part.
///   2. A bare number in an inch document meaning millimetres. That is a 25.4x error that looks
///      like a typo in the model rather than a bug in the parser.
///   3. `width * height` reaching a length field as a number. Areas and lengths are both doubles,
///      and the only thing between them is the dimension check.
///
/// So the cases below are mostly about UNITS AND DIMENSIONS, not about arithmetic. That 2+3 is 5 is
/// not in doubt; that 2+3 in an inch document is 127mm is the whole question.

#include "cad/expr/Expression.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <map>
#include <string>

using namespace cad;
using Catch::Approx;
using units::UnitSystem;

namespace {

/// Millimetres, or the error message if it did not evaluate. Every length assertion goes through
/// base units so that no test can accidentally agree with a wrong display conversion.
double mm(std::string_view text, UnitSystem system = UnitSystem::Millimetre,
          const expr::Resolver& resolver = {}) {
    const auto result = expr::evaluateLength(text, system, resolver);
    INFO("expression: " << text);
    REQUIRE(result.ok());
    return result.value().base();
}

std::string errorOf(std::string_view text, UnitSystem system = UnitSystem::Millimetre,
                    const expr::Resolver& resolver = {}) {
    const auto result = expr::evaluateLength(text, system, resolver);
    INFO("expression: " << text);
    REQUIRE_FALSE(result.ok());
    return result.error().message;
}

expr::Resolver table(std::map<std::string, expr::Value> values) {
    return [values = std::move(values)](std::string_view name) -> std::optional<expr::Value> {
        const auto found = values.find(std::string(name));
        if (found == values.end()) return std::nullopt;
        return found->second;
    };
}

expr::Value length(double millimetres) { return expr::Value{millimetres, expr::kLength, false}; }

}   // namespace

TEST_CASE("everything units::parseLength accepts still means the same thing", "[expr]") {
    // The evaluator REPLACES parseLength in every field the user types into, so it has to be a
    // strict superset. A regression here is not "expressions are broken" -- it is "typing 10 into a
    // box stopped working", which is every user, immediately.
    CHECK(mm("10") == Approx(10.0));
    CHECK(mm("10mm") == Approx(10.0));
    CHECK(mm("10 mm") == Approx(10.0));
    CHECK(mm("1.5in") == Approx(38.1));
    CHECK(mm("2'") == Approx(609.6));
    CHECK(mm("2ft 6in") == Approx(762.0));
    CHECK(mm("2' 6\"") == Approx(762.0));
    CHECK(mm("-3.25") == Approx(-3.25));
    CHECK(mm(".5") == Approx(0.5));
}

TEST_CASE("a bare number means the document's display unit", "[expr]") {
    // The 25.4x bug, pinned. `10` in an inch document is a quarter of a metre, and anything that
    // quietly returns 10mm has scaled the user's part by 1/25.4 without telling them.
    CHECK(mm("10", UnitSystem::Inch) == Approx(254.0));
    CHECK(mm("10", UnitSystem::Metre) == Approx(10000.0));
    CHECK(mm("10", UnitSystem::Centimetre) == Approx(100.0));

    // A suffix always wins over the display unit -- that is what typing one is for.
    CHECK(mm("10mm", UnitSystem::Inch) == Approx(10.0));
}

TEST_CASE("a bare number multiplies but does not add its own unit", "[expr][units]") {
    // The two rules that cannot be the same rule. See Expression.h.
    const auto width = table({{"width", length(40.0)}});

    CHECK(mm("width * 2", UnitSystem::Millimetre, width) == Approx(80.0));
    CHECK(mm("width + 10", UnitSystem::Millimetre, width) == Approx(50.0));
    CHECK(mm("width / 2", UnitSystem::Millimetre, width) == Approx(20.0));
    CHECK(mm("2 * width", UnitSystem::Millimetre, width) == Approx(80.0));

    // And the addition case follows the display unit, exactly as a bare number on its own does.
    CHECK(mm("width + 1", UnitSystem::Inch, width) == Approx(65.4));
    // While the multiplication case has no unit to follow.
    CHECK(mm("width * 3", UnitSystem::Inch, width) == Approx(120.0));
}

TEST_CASE("arithmetic on bare numbers stays bare until a field asks", "[expr][units]") {
    CHECK(mm("10 + 20") == Approx(30.0));
    CHECK(mm("10 + 20", UnitSystem::Inch) == Approx(762.0));   // 30in, not 30mm
    CHECK(mm("(2 + 3) * 4", UnitSystem::Inch) == Approx(508.0));
}

TEST_CASE("precedence and associativity are the ones everyone already knows", "[expr]") {
    CHECK(mm("2 + 3 * 4") == Approx(14.0));
    CHECK(mm("(2 + 3) * 4") == Approx(20.0));
    CHECK(mm("2 - 3 - 4") == Approx(-5.0));       // left
    CHECK(mm("2 ^ 3 ^ 2") == Approx(512.0));      // right
    CHECK(mm("-2 ^ 2") == Approx(4.0));           // unary binds the whole power here
    CHECK(mm("10 / 2 / 5") == Approx(1.0));
}

TEST_CASE("dimensions are checked, and a mismatch is refused rather than coerced", "[expr][units]") {
    const auto values = table({{"width", length(40.0)},
                               {"tilt", expr::Value{0.5, expr::kAngle, false}}});

    CHECK(errorOf("width + tilt", UnitSystem::Millimetre, values).find("add") != std::string::npos);

    // An area is not a length. Both are doubles; only the dimension separates them, and a field
    // that took this would produce a part 1600mm long from two 40mm inputs.
    const auto area = errorOf("width * width", UnitSystem::Millimetre, values);
    INFO(area);
    CHECK(area.find("mm^2") != std::string::npos);

    // The reverse direction too: a length is not an angle.
    const auto angle = expr::evaluateAngle("width", values);
    REQUIRE_FALSE(angle.ok());
    CHECK(angle.error().message.find("mm") != std::string::npos);

    // And a length is not a count.
    CHECK_FALSE(expr::evaluateNumber("width", values).ok());
}

TEST_CASE("dividing out a dimension gives back a plain number", "[expr][units]") {
    const auto values = table({{"width", length(40.0)}, {"pitch", length(8.0)}});
    const auto count = expr::evaluateNumber("width / pitch", values);
    REQUIRE(count.ok());
    CHECK(count.value() == Approx(5.0));
}

TEST_CASE("angles carry their own dimension", "[expr][units]") {
    const auto degrees = expr::evaluateAngle("45");
    REQUIRE(degrees.ok());
    CHECK(units::toDegrees(degrees.value()) == Approx(45.0));

    const auto suffixed = expr::evaluateAngle("0.5rad");
    REQUIRE(suffixed.ok());
    CHECK(suffixed.value().base() == Approx(0.5));

    const auto sum = expr::evaluateAngle("30deg + 15deg");
    REQUIRE(sum.ok());
    CHECK(units::toDegrees(sum.value()) == Approx(45.0));

    // Bare numbers in an angle expression are degrees, matching units::parseAngle.
    const auto mixed = expr::evaluateAngle("30deg + 15");
    REQUIRE(mixed.ok());
    CHECK(units::toDegrees(mixed.value()) == Approx(45.0));
}

TEST_CASE("trigonometry takes an angle and gives back a number", "[expr][units]") {
    const auto values = table({{"width", length(40.0)}});
    CHECK(mm("100 * sin(30deg)") == Approx(50.0));
    CHECK(mm("100 * sin(30)") == Approx(50.0));       // bare is degrees, not radians
    CHECK(mm("width * cos(0)", UnitSystem::Millimetre, values) == Approx(40.0));

    // sin of a length is the mistake the dimension check exists to catch.
    const auto bad = errorOf("sin(width)", UnitSystem::Millimetre, values);
    INFO(bad);
    CHECK(bad.find("angle") != std::string::npos);

    // The inverse functions go the other way.
    const auto angle = expr::evaluateAngle("atan2(1, 1)");
    REQUIRE(angle.ok());
    CHECK(units::toDegrees(angle.value()) == Approx(45.0));
}

TEST_CASE("sqrt halves the dimension", "[expr][units]") {
    const auto values = table({{"area", expr::Value{400.0, expr::Dim{2, 0, 0, 0}, false}}});
    CHECK(mm("sqrt(area)", UnitSystem::Millimetre, values) == Approx(20.0));

    // But the square root of a length is not a unit anything can hold.
    const auto bad = errorOf("sqrt(10mm)");
    INFO(bad);
    CHECK(bad.find("mm") != std::string::npos);
}

TEST_CASE("names resolve through the caller, and unknown ones say so by name", "[expr]") {
    const auto values = table({{"width", length(40.0)}});
    CHECK(mm("width", UnitSystem::Millimetre, values) == Approx(40.0));

    const auto typo = errorOf("widht", UnitSystem::Millimetre, values);
    INFO(typo);
    // Naming the offending word is the difference between a usable message and "syntax error".
    CHECK(typo.find("widht") != std::string::npos);

    // With no table at all -- a field in a context that has no parameters -- the message should say
    // that, rather than blaming the name.
    const auto none = errorOf("width");
    INFO(none);
    CHECK(none.find("width") != std::string::npos);
}

TEST_CASE("the dependencies of an expression can be read without evaluating it", "[expr]") {
    // What cycle detection is built on: `a = b + 1` has to be known to depend on b BEFORE anything
    // tries to compute a. Asking for the value first is how an evaluator recurses to a stack
    // overflow instead of reporting a loop.
    const auto names = expr::referencedNames("width * 2 + height - width");
    REQUIRE(names.ok());
    REQUIRE(names.value().size() == 2);
    CHECK(names.value()[0] == "width");     // first-use order, deduplicated
    CHECK(names.value()[1] == "height");

    // Functions and constants are not dependencies.
    const auto builtins = expr::referencedNames("sin(30deg) * pi + bore");
    REQUIRE(builtins.ok());
    REQUIRE(builtins.value().size() == 1);
    CHECK(builtins.value()[0] == "bore");

    // Reading dependencies must not require the names to exist.
    const auto unknown = expr::referencedNames("nothing_defined_yet + 1");
    REQUIRE(unknown.ok());
    CHECK(unknown.value().size() == 1);
}

TEST_CASE("bad input is refused with a message that points at the problem", "[expr]") {
    CHECK(errorOf("").find("Enter") != std::string::npos);
    CHECK(errorOf("2 +").find("incomplete") != std::string::npos);
    CHECK(errorOf("(2 + 3").find("closed") != std::string::npos);
    CHECK(errorOf("2 3").find("extra") != std::string::npos);
    // A detached word is only a unit if it is one. `2 width` must not silently become a product.
    CHECK(errorOf("2 width", UnitSystem::Millimetre,
                  table({{"width", length(40.0)}})).find("extra") != std::string::npos);
    CHECK(errorOf("10 / 0").find("zero") != std::string::npos);
    CHECK(errorOf("10 $ 2").find("$") != std::string::npos);
    CHECK(errorOf("10furlongs").find("furlongs") != std::string::npos);
    CHECK(errorOf("nosuchfn(2)").find("nosuchfn") != std::string::npos);
    CHECK(errorOf("min(2)").find("2 values") != std::string::npos);
    CHECK(errorOf("sin").find("brackets") != std::string::npos);
}

TEST_CASE("a parameter may not be named after a built-in", "[expr]") {
    CHECK(expr::isValidName("width"));
    CHECK(expr::isValidName("bore_2"));
    CHECK(expr::isValidName("_private"));
    CHECK_FALSE(expr::isValidName(""));
    CHECK_FALSE(expr::isValidName("2wide"));
    CHECK_FALSE(expr::isValidName("wall thickness"));
    CHECK_FALSE(expr::isValidName("width-2"));
    // Shadowing pi or sin is worse than being told to pick another name.
    CHECK_FALSE(expr::isValidName("pi"));
    CHECK_FALSE(expr::isValidName("sin"));
    CHECK_FALSE(expr::isValidName("PI"));
    // Units are reserved for the same reason: "2 in" is two inches everywhere else.
    CHECK_FALSE(expr::isValidName("in"));
    CHECK_FALSE(expr::isValidName("mm"));
    CHECK_FALSE(expr::isValidName("deg"));
}

TEST_CASE("an expression a machinist would actually type", "[expr]") {
    // Nothing exotic: a bolt circle position, in a document set to inches, from parameters that
    // are stored in millimetres like everything else.
    const auto values = table({{"bolt_circle", length(101.6)},   // 4in
                               {"count", expr::Value{6.0, expr::kScalar, false}}});
    const auto resolver = values;

    CHECK(mm("bolt_circle / 2", UnitSystem::Inch, resolver) == Approx(50.8));
    CHECK(mm("bolt_circle / 2 * cos(360deg / count)", UnitSystem::Inch, resolver)
          == Approx(25.4));
    // The same expression written with a bare number where a length belongs.
    CHECK(mm("bolt_circle / 2 + 0.125", UnitSystem::Inch, resolver) == Approx(50.8 + 3.175));
}
