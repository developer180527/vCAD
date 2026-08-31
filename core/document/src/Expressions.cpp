#include "cad/document/Expressions.h"

#include <cmath>
#include <utility>

namespace cad::document {
namespace {

/// Evaluates one expression into the SAME kind of value the property already holds.
///
/// The property's current type is what decides which evaluation is asked for, rather than whatever
/// the expression happens to work out to. A depth is a length whatever the user typed into it, so
/// `width * height` in a depth field has to be refused as an area -- and it is expr:: that refuses
/// it, because that is where the dimension rules live.
base::Result<PropertyValue> evaluateAs(const PropertyValue& current, const std::string& text,
                                       units::UnitSystem enteredIn,
                                       const expr::Resolver& resolver) {
    switch (typeOf(current)) {
        case PropertyType::Length: {
            const auto value = expr::evaluateLength(text, enteredIn, resolver);
            if (!value.ok()) return value.error();
            return PropertyValue{value.value()};
        }
        case PropertyType::Angle: {
            const auto value = expr::evaluateAngle(text, resolver);
            if (!value.ok()) return value.error();
            return PropertyValue{value.value()};
        }
        case PropertyType::Real: {
            const auto value = expr::evaluateNumber(text, resolver);
            if (!value.ok()) return value.error();
            return PropertyValue{value.value()};
        }
        case PropertyType::Int: {
            const auto value = expr::evaluateNumber(text, resolver);
            if (!value.ok()) return value.error();
            // A count of 5.5 holes is not a count. Rounding silently would produce a part with a
            // different number of features than the expression says.
            const double rounded = std::round(value.value());
            if (std::abs(value.value() - rounded) > 1e-9) {
                return base::Error{base::ErrorCode::InvalidInput,
                                   "This needs a whole number, but that works out to "
                                       + std::to_string(value.value()) + "."};
            }
            return PropertyValue{static_cast<std::int64_t>(rounded)};
        }
        default:
            // Bool, Text, and every geometric reference. An expression cannot produce a face, and
            // a property that somehow carries one alongside expression text is a bug upstream of
            // here, not something to paper over with a conversion.
            return base::Error{base::ErrorCode::InvalidInput,
                               "An expression can't produce " + std::string(toString(typeOf(current)))
                                   + "."};
    }
}

}   // namespace

Reevaluation reevaluate(const Document& source, const expr::Resolver& resolver) {
    Reevaluation out{source, 0, {}};

    for (const ObjectId id : source.ids()) {
        const auto object = source.find(id);
        if (!object) continue;

        ObjectData updated = *object;
        bool touched = false;
        std::string firstProblem;

        for (const auto& property : object->properties()) {
            if (property.expression.empty()) continue;

            auto value = evaluateAs(property.value, property.expression, property.expressionUnits,
                                    resolver);
            if (!value.ok()) {
                out.problems.push_back(ExpressionProblem{id, property.name, property.expression,
                                                         value.error().message});
                if (firstProblem.empty()) {
                    firstProblem = property.name + ": " + value.error().message;
                }
                continue;   // keep the last good value; see the header
            }
            if (digestOf(value.value()) == digestOf(property.value)) continue;

            updated = updated.withExpression(property.name, std::move(value.value()),
                                             property.expression, property.expressionUnits,
                                             property.cosmetic);
            touched = true;
            ++out.changed;
        }

        if (!firstProblem.empty()) {
            updated = updated.withError(
                kernel::Error{kernel::ErrorCode::InvalidInput, std::move(firstProblem)});
            touched = true;
        } else if (touched) {
            // Dirty, not recomputed. Deciding what a changed property does to geometry is the
            // recompute engine's job, and doing any of it here would be a second, quieter copy of
            // its dependency rules.
            updated = updated.withState(ObjectState::Dirty);
        }

        if (touched) out.document = out.document.replace(std::make_shared<const ObjectData>(std::move(updated)));
    }

    return out;
}

}  // namespace cad::document
