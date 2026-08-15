#include "cad/units/Units.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace cad::units {
namespace {

using base::Error;
using base::ErrorCode;

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

std::string lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

/// Splits a leading number from a trailing suffix. Returns false if there is no number.
bool splitNumber(std::string_view text, double& value, std::string_view& rest) {
    const std::string buf(text);
    char* end = nullptr;
    const double v = std::strtod(buf.c_str(), &end);
    if (end == buf.c_str()) return false;
    value = v;
    rest = trim(text.substr(static_cast<std::size_t>(end - buf.c_str())));
    return true;
}

/// Length suffix -> millimetres per unit. Empty suffix is handled by the caller, which
/// knows the assumed system.
bool lengthFactor(std::string_view suffix, double& factor) {
    const std::string s = lower(suffix);
    if (s == "mm" || s == "millimetre" || s == "millimeter") { factor = 1.0;      return true; }
    if (s == "cm" || s == "centimetre" || s == "centimeter") { factor = 10.0;     return true; }
    if (s == "m"  || s == "metre"      || s == "meter")      { factor = 1000.0;   return true; }
    if (s == "um" || s == "micron")                          { factor = 0.001;    return true; }
    if (s == "in" || s == "inch" || s == "inches" || s == "\"") { factor = 25.4;  return true; }
    if (s == "ft" || s == "foot" || s == "feet" || s == "'")    { factor = 304.8; return true; }
    if (s == "thou" || s == "mil")                           { factor = 0.0254;   return true; }
    return false;
}

double systemFactor(UnitSystem u) {
    switch (u) {
        case UnitSystem::Millimetre: return 1.0;
        case UnitSystem::Centimetre: return 10.0;
        case UnitSystem::Metre:      return 1000.0;
        case UnitSystem::Inch:       return 25.4;
        case UnitSystem::Foot:       return 304.8;
    }
    return 1.0;
}

}  // namespace

const char* suffix(UnitSystem u) noexcept {
    switch (u) {
        case UnitSystem::Millimetre: return "mm";
        case UnitSystem::Centimetre: return "cm";
        case UnitSystem::Metre:      return "m";
        case UnitSystem::Inch:       return "in";
        case UnitSystem::Foot:       return "ft";
    }
    return "mm";
}

base::Result<Length> parseLength(std::string_view text, UnitSystem assumed) {
    std::string_view s = trim(text);
    if (s.empty()) {
        return Error{ErrorCode::InvalidInput, "Enter a length."};
    }

    double total = 0.0;
    bool any = false;

    // Loop so "2ft 6in" and "2' 6\"" work — still normal in US mechanical drawings.
    while (!s.empty()) {
        double value = 0.0;
        std::string_view rest;
        if (!splitNumber(s, value, rest)) {
            return Error{ErrorCode::InvalidInput,
                         "'" + std::string(trim(text)) + "' is not a valid length."};
        }

        // The unit suffix runs to the next digit, sign, or space.
        std::size_t n = 0;
        while (n < rest.size() && !std::isdigit(static_cast<unsigned char>(rest[n]))
               && rest[n] != '-' && rest[n] != '+' && !std::isspace(static_cast<unsigned char>(rest[n]))) {
            ++n;
        }
        const std::string_view unit = rest.substr(0, n);

        double factor = 0.0;
        if (unit.empty()) {
            factor = systemFactor(assumed);
        } else if (!lengthFactor(unit, factor)) {
            return Error{ErrorCode::InvalidInput,
                         "'" + std::string(unit) + "' is not a unit I recognise."};
        }
        total += value * factor;
        any = true;
        s = trim(rest.substr(n));
    }

    if (!any) return Error{ErrorCode::InvalidInput, "Enter a length."};
    return Length::fromBase(total);
}

base::Result<Angle> parseAngle(std::string_view text) {
    std::string_view s = trim(text);
    if (s.empty()) return Error{ErrorCode::InvalidInput, "Enter an angle."};

    double value = 0.0;
    std::string_view rest;
    if (!splitNumber(s, value, rest)) {
        return Error{ErrorCode::InvalidInput,
                     "'" + std::string(s) + "' is not a valid angle."};
    }
    const std::string unit = lower(trim(rest));
    if (unit.empty() || unit == "deg" || unit == "degrees" || unit == "\xc2\xb0") {
        return degrees(value);
    }
    if (unit == "rad" || unit == "radians") {
        return radians(value);
    }
    return Error{ErrorCode::InvalidInput,
                 "'" + unit + "' is not an angle unit I recognise."};
}

std::string format(Length l, UnitSystem u, int decimals) {
    if (decimals < 0) {
        // Default precision is chosen so that one displayed digit is <= 1 micron, i.e. the
        // text round-trips back through parseLength without losing anything a CAD user
        // would care about. A user who copies a value out of a field and pastes it back
        // must not silently move their model.
        //
        // Naive per-system defaults (4 everywhere) fail this: 0.1235 m is 123.5 mm, a 44
        // micron error. Callers that want prettier display pass `decimals` explicitly.
        switch (u) {
            case UnitSystem::Millimetre: decimals = 3; break;  // 0.001 mm
            case UnitSystem::Centimetre: decimals = 4; break;  // 0.001 mm
            case UnitSystem::Metre:      decimals = 6; break;  // 0.001 mm
            case UnitSystem::Inch:       decimals = 5; break;  // 0.00025 mm
            case UnitSystem::Foot:       decimals = 6; break;  // 0.0003 mm
        }
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f %s", decimals, l.base() / systemFactor(u), suffix(u));
    return buf;
}

std::string format(Angle a, int decimals) {
    if (decimals < 0) decimals = 2;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f\xc2\xb0", decimals, toDegrees(a));
    return buf;
}

base::Result<double> scaleToMillimetres(std::string_view unitName) {
    double factor = 0.0;
    if (lengthFactor(trim(unitName), factor)) return factor;
    return Error{ErrorCode::Unsupported,
                 "This file declares units I don't recognise ('" + std::string(unitName) +
                     "'), so I can't safely scale it.",
                 "unknown unit name at import"};
}

}  // namespace cad::units
