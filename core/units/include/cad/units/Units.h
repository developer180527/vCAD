#pragma once

// Runtime side of the unit system: parsing user input and formatting for display.
//
// The compile-time half (Quantity.h) is what the core computes with. This half exists only
// at the boundary — the property editor, dimension input, file import/export. Keeping the
// two separate is deliberate: no part of the geometry pipeline should ever consult a
// user-facing unit preference.

#include "cad/base/Result.h"
#include "cad/units/Quantity.h"

#include <string>
#include <string_view>

namespace cad::units {

/// What the user sees. Storage and computation are always mm/rad regardless.
enum class UnitSystem {
    Millimetre,   ///< mm — the default, and what STEP/3MF usually carry
    Centimetre,
    Metre,
    Inch,
    Foot,
};

const char* suffix(UnitSystem) noexcept;

/// Parses a length with an optional unit suffix: "10", "10mm", "10 mm", "1.5in", "2'".
/// With no suffix the value is interpreted in `assumed` — which is why `assumed` has no
/// default. Guessing units silently is how a 25.4x scaling bug reaches a customer.
///
/// Also accepts feet-and-inches ("2ft 6in", "2' 6\"") because mechanical drawings in the
/// US market still use it.
base::Result<Length> parseLength(std::string_view text, UnitSystem assumed);

/// Parses an angle: "45", "45deg", "45°", "0.785rad". Bare values are degrees — unlike
/// length there is a genuine convention here, and radians are always suffixed.
base::Result<Angle> parseAngle(std::string_view text);

/// Formats for display. `decimals` < 0 selects a sensible default per system.
std::string format(Length, UnitSystem, int decimals = -1);
std::string format(Angle, int decimals = -1);

/// Scale factor from a foreign file's unit into our base (mm). Used at import.
/// Returns an error rather than 1.0 for an unrecognised unit name: a wrong scale is worse
/// than a refused import (docs/FORMATS.md rule 2).
base::Result<double> scaleToMillimetres(std::string_view unitName);

}  // namespace cad::units
