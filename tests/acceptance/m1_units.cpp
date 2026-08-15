// Units. Present in M1 because retrofitting them later is agony, and because the parse
// boundary is where a silent 25.4x scaling bug reaches a customer.

#include "cad/units/Units.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace cad::units;
using namespace cad::units::literals;
using Catch::Approx;

TEST_CASE("M1: quantities convert through a single base unit", "[m1][units]") {
    CHECK(toMillimetres(1.0_in) == Approx(25.4));
    CHECK(toMillimetres(1.0_ft) == Approx(304.8));
    CHECK(toMillimetres(1.0_m) == Approx(1000.0));
    CHECK(toInches(25.4_mm) == Approx(1.0));
    CHECK(toDegrees(180.0_deg) == Approx(180.0));
    CHECK(toRadians(180.0_deg) == Approx(3.14159265358979));
}

TEST_CASE("M1: dimensional arithmetic composes", "[m1][units]") {
    const Length a = 10.0_mm;
    const Length b = 20.0_mm;

    CHECK(toMillimetres(a + b) == Approx(30.0));
    CHECK(toMillimetres(b - a) == Approx(10.0));
    CHECK(toMillimetres(a * 3.0) == Approx(30.0));
    CHECK((b / a) == Approx(2.0));           // same dimension -> plain ratio

    const Area area = a * b;                 // mm * mm -> mm²
    CHECK(area.base() == Approx(200.0));
    const Volume vol = area * a;             // mm² * mm -> mm³
    CHECK(vol.base() == Approx(2000.0));
    const Length back = area / a;            // mm² / mm -> mm
    CHECK(toMillimetres(back) == Approx(20.0));

    // These must NOT compile, which is the entire point of the type:
    //   Length x = a + 45.0_deg;
    //   Length y = area;
    //   double z = a + 1.0;
    STATIC_REQUIRE(std::is_same_v<decltype(a * b), Area>);
    STATIC_REQUIRE(std::is_same_v<decltype(area / a), Length>);
    STATIC_REQUIRE(!std::is_same_v<Length, Angle>);
}

TEST_CASE("M1: length parsing never guesses units", "[m1][units]") {
    // A bare number takes the caller's declared system — the caller must state it.
    CHECK(toMillimetres(parseLength("10", UnitSystem::Millimetre).value()) == Approx(10.0));
    CHECK(toMillimetres(parseLength("10", UnitSystem::Inch).value()) == Approx(254.0));

    CHECK(toMillimetres(parseLength("10mm", UnitSystem::Inch).value()) == Approx(10.0));
    CHECK(toMillimetres(parseLength("1.5 in", UnitSystem::Millimetre).value()) == Approx(38.1));
    CHECK(toMillimetres(parseLength("2m", UnitSystem::Millimetre).value()) == Approx(2000.0));
    CHECK(toMillimetres(parseLength("  -3.5 cm ", UnitSystem::Metre).value()) == Approx(-35.0));

    // Feet and inches together, both spellings — still normal on US drawings.
    CHECK(toMillimetres(parseLength("2ft 6in", UnitSystem::Millimetre).value()) == Approx(762.0));
    CHECK(toMillimetres(parseLength("2' 6\"", UnitSystem::Millimetre).value()) == Approx(762.0));

    // Garbage is refused, not coerced to zero.
    CHECK_FALSE(parseLength("", UnitSystem::Millimetre).ok());
    CHECK_FALSE(parseLength("abc", UnitSystem::Millimetre).ok());
    CHECK_FALSE(parseLength("10 furlongs", UnitSystem::Millimetre).ok());
}

TEST_CASE("M1: angle parsing defaults to degrees", "[m1][units]") {
    CHECK(toDegrees(parseAngle("45").value()) == Approx(45.0));
    CHECK(toDegrees(parseAngle("45deg").value()) == Approx(45.0));
    CHECK(toDegrees(parseAngle("45\xc2\xb0").value()) == Approx(45.0));
    CHECK(toDegrees(parseAngle("3.14159265358979 rad").value()) == Approx(180.0));
    CHECK_FALSE(parseAngle("45 gradians").ok());
}

TEST_CASE("M1: parse and format round-trip to within a micron", "[m1][units]") {
    // A user who copies a value out of a field and pastes it back must not move their
    // model. That constrains the DEFAULT display precision, which is why format() picks
    // per-system decimals rather than one number for all of them.
    for (auto sys : {UnitSystem::Millimetre, UnitSystem::Centimetre, UnitSystem::Metre,
                     UnitSystem::Inch, UnitSystem::Foot}) {
        for (double mm : {123.456, 0.05, 9999.999, -42.125}) {
            const Length original = millimetres(mm);
            const std::string text = format(original, sys);
            INFO(text << "  (system " << suffix(sys) << ")");
            auto parsed = parseLength(text, sys);
            REQUIRE(parsed.ok());
            CHECK(toMillimetres(parsed.value())
                  == Approx(toMillimetres(original)).margin(0.001));
        }
    }
}

TEST_CASE("M1: angles round-trip", "[m1][units]") {
    for (double deg : {0.0, 45.0, 90.0, 123.45, -30.0}) {
        const Angle original = degrees(deg);
        auto parsed = parseAngle(format(original, 6));
        REQUIRE(parsed.ok());
        CHECK(toDegrees(parsed.value()) == Approx(deg).margin(1e-5));
    }
}

TEST_CASE("M1: an unknown import unit is refused, not assumed", "[m1][units]") {
    CHECK(scaleToMillimetres("mm").value() == Approx(1.0));
    CHECK(scaleToMillimetres("INCH").value() == Approx(25.4));
    // docs/FORMATS.md rule 2: a wrong scale is worse than a refused import.
    auto bad = scaleToMillimetres("smoots");
    REQUIRE_FALSE(bad.ok());
    CHECK(bad.error().code == cad::base::ErrorCode::Unsupported);
}
