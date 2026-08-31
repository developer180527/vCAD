#pragma once

#include "cad/naming/ElementName.h"
#include "cad/units/Quantity.h"
#include "cad/units/Units.h"

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace cad::document {

/// Object identity within a document. Stable for the object's lifetime, persisted, and
/// crossing the C ABI. Not an index — deleting an object never renumbers the others.
struct ObjectId {
    std::uint64_t value = 0;

    [[nodiscard]] bool isNull() const noexcept { return value == 0; }
    friend auto operator<=>(const ObjectId&, const ObjectId&) = default;
    friend bool operator==(const ObjectId&, const ObjectId&) = default;
};

/// The complete set of things a feature parameter can be.
///
/// Deliberately closed. An open `std::any`-style property system means the cache key, the
/// serializer, and the undo machinery each need an escape hatch, and each escape hatch is a
/// place where a change silently fails to invalidate something. Adding a case here is a
/// deliberate act that makes you update all three.
///
/// Note that geometric references are `ElementName`, never an index — that is the whole
/// point of M1 and it has to be visible in the type system here, or feature authors will
/// reach for an integer.
using PropertyValue = std::variant<
    bool,
    std::int64_t,
    double,
    std::string,
    units::Length,
    units::Angle,
    ObjectId,                            ///< a reference to another object in the document
    naming::ElementName,                 ///< a reference to one face/edge/vertex
    std::vector<naming::ElementName>,    ///< …or several
    std::vector<ObjectId>>;

enum class PropertyType : std::uint8_t {
    Bool, Int, Real, Text, Length, Angle, Object, Element, ElementList, ObjectList
};

PropertyType typeOf(const PropertyValue&) noexcept;
const char* toString(PropertyType) noexcept;

/// A named parameter on an object.
struct Property {
    std::string name;

    /// What the feature computes from. ALWAYS the evaluated result, in base units. Nothing
    /// downstream of here — no feature, no cache key, no exporter — needs to know whether a human
    /// typed this number or an expression produced it, and that is what keeps expressions from
    /// touching the feature catalogue at all.
    PropertyValue value;

    /// Excluded from the recompute cache key. For things that genuinely do not affect
    /// geometry — display colour, a user label. Getting this wrong in the "true" direction
    /// causes stale geometry, so the default is false and every exclusion needs a reason.
    bool cosmetic = false;

    /// The text the user typed, when it was more than a number: "width * 2", "bore/2 + 0.5mm".
    /// Empty when the value is just a value, which is the overwhelmingly common case.
    ///
    /// This is stored rather than thrown away because a model that keeps only the 80 that
    /// `width * 2` evaluated to is not parametric. Changing `width` would move nothing, and
    /// reopening the file would show a plain number where a relationship used to be — the link
    /// silently gone, with the model still looking correct.
    std::string expression;

    /// The display unit in force when `expression` was typed, and the unit a BARE NUMBER inside it
    /// means forever afterwards.
    ///
    /// Not a detail. Display units are a user PREFERENCE, not a document property — a colleague
    /// opening your file gets their own. So an expression of `width + 10` stored without this,
    /// re-evaluated by someone whose preference is inches, would silently become `width + 254mm`.
    /// Geometry that depends on who opened the file is the worst class of bug this format could
    /// have, and one column prevents it.
    ///
    /// Meaningless when `expression` is empty.
    units::UnitSystem expressionUnits = units::UnitSystem::Millimetre;
};

/// Folded into the recompute cache key. Must be a pure function of the value, stable across
/// processes and machines — same discipline as naming digests.
std::uint64_t digestOf(const PropertyValue&) noexcept;

/// Human-readable, for diagnostics and the property inspector.
std::string toString(const PropertyValue&);

}  // namespace cad::document

template <>
struct std::hash<cad::document::ObjectId> {
    std::size_t operator()(const cad::document::ObjectId& id) const noexcept {
        return static_cast<std::size_t>(id.value);
    }
};
