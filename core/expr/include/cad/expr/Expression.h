#pragma once

// Expressions: what a user may type where a number is expected.
//
// `10`, `10mm`, `2ft 6in`, `width`, `width * 2`, `thickness + 0.5mm`, `bore/2 + clearance`,
// `100 * sin(30deg)`. This is the layer under named parameters, and it exists before them
// deliberately: a parameter table whose values are plain doubles is a renaming exercise, not a
// modelling feature. What makes parameters worth having is that one edit propagates, and that only
// works if a property can hold "width * 2" rather than the 40 it happened to evaluate to.
//
// # Why this is not a bigger part of units/
//
// core/units is about the boundary: turning "1.5in" into millimetres and back. That is a closed
// job with no notion of a name, a document, or a dependency. Expressions need all three -- an
// identifier means whatever the document's parameter table says today -- so they are their own
// module that units cannot depend on. `parseLength` stays exactly as it is and stays correct;
// `evaluateLength` is a superset that also accepts arithmetic and names.
//
// # Dimensions are checked, not assumed
//
// Quantity.h checks dimensions at compile time, which is the right tool for code we write. User
// text arrives at runtime, so the check moves with it: a Value carries the exponents of its base
// dimensions and the evaluator enforces them. `width + angle` is refused. `sin(10mm)` is refused.
// `width * height` produces an area, which a length field then refuses -- with a message saying so,
// rather than silently taking the number.
//
// This costs a few lines and catches the class of mistake that units exist to catch. Evaluating to
// a bare double and hoping would make the whole module ornamental.
//
// # Bare numbers
//
// A number with no suffix is UNITLESS -- not a length, not a scalar, but a value that has not yet
// been told what it is. It adopts a dimension when it meets one:
//
//     width * 2       the 2 stays a pure multiplier          -> a length
//     width + 10      the 10 becomes 10 display units        -> a length
//     10 + 20         nothing tells it; the caller decides   -> 30 display units, in a length field
//
// This is the rule Fusion and SolidWorks both use, and it is the only rule under which both
// `width*2` and `width+10` mean what a mechanical engineer expects. Treating bare numbers as
// millimetres breaks the first; treating them as scalars breaks the second.

#include "cad/base/Result.h"
#include "cad/units/Quantity.h"
#include "cad/units/Units.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cad::expr {

/// Runtime mirror of units::Dimension. Carries all four base exponents even though only length and
/// angle are reachable from today's syntax: the compile-time type has four, and a runtime mirror
/// that quietly has fewer is a mirror that stops reflecting the moment someone adds a mass.
struct Dim {
    int length = 0;
    int mass = 0;
    int time = 0;
    int angle = 0;

    friend bool operator==(const Dim&, const Dim&) = default;
    [[nodiscard]] bool isScalar() const noexcept { return *this == Dim{}; }
};

inline constexpr Dim kScalar{};
inline constexpr Dim kLength{1, 0, 0, 0};
inline constexpr Dim kAngle{0, 0, 0, 1};

/// For error messages: "mm", "rad", "mm^2", "mm/rad". Never shown as a value's unit -- only to
/// explain a mismatch, which is the one moment a user needs to see the machinery.
std::string toString(Dim);

/// One value in an expression. Magnitude is always in BASE units (millimetres, radians), the same
/// contract as Quantity, so nothing in the middle of an evaluation ever consults a preference.
struct Value {
    double magnitude = 0.0;
    Dim dim;

    /// True only for a number typed with no unit suffix, and only while it has not been combined
    /// with something dimensioned. `magnitude` then holds the number AS TYPED -- 10 stays 10, not
    /// 254 -- because what it converts to depends on the dimension it eventually adopts.
    bool unitless = false;
};

/// How a name is looked up. Returns nothing for a name the caller does not know, which the
/// evaluator turns into "There is no parameter called 'widht'."
///
/// A callback rather than a map so that the document's parameter table can stay in core/document
/// and evaluate lazily -- a parameter whose own expression mentions another parameter resolves
/// through the same path, and cycle detection stays the caller's business where the graph is.
using Resolver = std::function<std::optional<Value>(std::string_view)>;

/// Evaluates. `assumedLength` is used only to interpret a bare number that ends up in a length
/// context; it never affects a value that carried its own suffix.
base::Result<Value> evaluate(std::string_view text, units::UnitSystem assumedLength,
                             const Resolver& resolver = {});

/// Evaluates and requires the result to be a length. A unitless result adopts `assumed`, which
/// makes this a strict superset of units::parseLength -- every input that parses today evaluates
/// to the same value.
base::Result<units::Length> evaluateLength(std::string_view text, units::UnitSystem assumed,
                                           const Resolver& resolver = {});

/// Evaluates and requires an angle. A unitless result is degrees, matching units::parseAngle.
base::Result<units::Angle> evaluateAngle(std::string_view text, const Resolver& resolver = {});

/// Evaluates and requires a plain number: a count, a ratio, a scale factor.
base::Result<double> evaluateNumber(std::string_view text, const Resolver& resolver = {});

/// The names an expression mentions, in first-use order, without duplicates.
///
/// Separate from evaluation because the dependency graph has to be known BEFORE a value can be
/// asked for: that is what makes `a = b + 1` / `b = a + 1` a reported cycle instead of a stack
/// overflow. Function names and built-in constants are not names in this sense and are excluded.
base::Result<std::vector<std::string>> referencedNames(std::string_view text);

/// Whether a string may be used as a parameter name. Letters, digits and underscore, not starting
/// with a digit, and not a built-in (`pi`, `sin`, ...) -- a parameter named `pi` would be either
/// shadowed or shadowing, and both are worse than refusing the name.
[[nodiscard]] bool isValidName(std::string_view name);

}  // namespace cad::expr
