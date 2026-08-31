/// The application's side of named parameters: reading them for a table, and editing them.
///
/// The rules -- resolution order, cycles, dimensions, entry units -- all live in
/// core/document/Parameters.h. What is here is the part that only an application has: formatting
/// for display, deciding whether typed text is a value or a formula, and committing an edit to
/// history so it can be undone.

#include "Internal.h"

#include "cad/document/Parameters.h"
#include "cad/expr/Expression.h"

#include <cctype>
#include <cmath>
#include <cstdlib>

namespace cad::app {

/// Decided by asking the OLD grammar: if units::parseLength accepts it, it is a number with at
/// most a unit on it -- "40", "1.5in", "2ft 6in" -- and there is no relationship to preserve.
/// Anything else went through the expression evaluator to produce its value, so the text is what
/// the user actually meant and the number is only today's answer.
///
/// Asked this way round rather than by hunting for operators because the unit grammar already has
/// the awkward cases in it: a lone `"` is a unit, `2' 6"` contains a space and no operator, and a
/// leading minus is not arithmetic.
bool isPlainQuantity(const std::string& text, document::PropertyType type,
                     units::UnitSystem display) {
    switch (type) {
        case document::PropertyType::Length: return units::parseLength(text, display).ok();
        case document::PropertyType::Angle:  return units::parseAngle(text).ok();
        default: {
            // Real and Int: a plain number is one strtod consumes entirely.
            const std::string buf = text;
            char* end = nullptr;
            const double value = std::strtod(buf.c_str(), &end);
            (void)value;
            if (end == buf.c_str()) return false;
            while (*end != '\0' && std::isspace(static_cast<unsigned char>(*end)) != 0) ++end;
            return *end == '\0';
        }
    }
}

std::vector<Controller::ParameterRow> Controller::parameters() const {
    const auto& doc = history_.current();
    // Resolved ONCE for the whole table. Resolving per row would re-walk the dependency graph for
    // every line, which on a forty-parameter model is forty times the work to show one screen.
    const auto resolved = document::resolveParameters(doc);

    std::vector<ParameterRow> out;
    for (const auto& parameter : doc.parameters()) {
        ParameterRow row;
        row.name = parameter.name;
        row.expression = parameter.expression;

        const auto found = resolved.values.find(parameter.name);
        if (found != resolved.values.end()) {
            if (found->second.dim == expr::kAngle) {
                row.value = units::format(units::Angle::fromBase(found->second.magnitude));
            } else if (found->second.dim == expr::kLength) {
                row.value = units::format(units::Length::fromBase(found->second.magnitude),
                                          preferences_.displayUnits);
            } else {
                row.value = document::toString(document::PropertyValue{found->second.magnitude});
            }
        }
        for (const auto& problem : resolved.problems) {
            if (problem.name == parameter.name) row.problem = problem.message;
        }
        out.push_back(std::move(row));
    }
    return out;
}

bool Controller::setParameter(const std::string& name, const std::string& text) {
    const auto& doc = history_.current();
    const auto* existing = doc.parameter(name);

    if (existing == nullptr) {
        const auto problem = document::parameterNameProblem(doc, name);
        if (!problem.empty()) {
            status(problem);
            return false;
        }
    }

    const auto resolver = document::resolveParameters(doc).resolver();

    // The KIND. An existing parameter keeps the one it has -- retyping the value of a length
    // parameter must not turn it into a bare number because the new text has no unit on it. A new
    // one takes its kind from what the text works out to.
    document::PropertyType kind = document::PropertyType::Length;
    if (existing != nullptr) {
        kind = document::typeOf(existing->value);
    } else {
        const auto probe = expr::evaluate(text, preferences_.displayUnits, resolver);
        if (!probe.ok()) {
            status(probe.error().message);
            return false;
        }
        if (probe.value().dim == expr::kAngle) {
            kind = document::PropertyType::Angle;
        } else if (probe.value().dim.isScalar() && !probe.value().unitless) {
            // Scalar but not a bare number: the user wrote something that is genuinely a ratio or
            // a count, like `bore / pitch`. A bare `12` stays a length, which is what someone
            // typing a number into a new parameter almost always means.
            kind = document::PropertyType::Real;
        }
    }

    document::Property parameter;
    parameter.name = name;
    parameter.expressionUnits = preferences_.displayUnits;
    if (!isPlainQuantity(text, kind, preferences_.displayUnits)) parameter.expression = text;

    // The value, in the parameter's own kind. For an expression this is only the starting point --
    // rebuildFromParameters overwrites it with the resolved number -- but it still has to be the
    // right TYPE, because the type is what declares the parameter's kind.
    switch (kind) {
        case document::PropertyType::Angle: {
            const auto angle = expr::evaluateAngle(text, resolver);
            if (!angle.ok()) { status(angle.error().message); return false; }
            parameter.value = angle.value();
            break;
        }
        case document::PropertyType::Real: {
            const auto number = expr::evaluateNumber(text, resolver);
            if (!number.ok()) { status(number.error().message); return false; }
            parameter.value = number.value();
            break;
        }
        case document::PropertyType::Int: {
            const auto number = expr::evaluateNumber(text, resolver);
            if (!number.ok()) { status(number.error().message); return false; }
            parameter.value = static_cast<std::int64_t>(std::llround(number.value()));
            break;
        }
        default: {
            const auto length = expr::evaluateLength(text, preferences_.displayUnits, resolver);
            if (!length.ok()) { status(length.error().message); return false; }
            parameter.value = length.value();
            break;
        }
    }

    // Tried on a CANDIDATE document. A cycle or a typo must leave the model exactly as it was:
    // storing it and reporting the problem afterwards produces a document whose parameters cannot
    // all be resolved, which is a state the user did not ask for and cannot easily get out of.
    const auto candidate = doc.withParameter(parameter);
    for (const auto& problem : document::resolveParameters(candidate).problems) {
        if (problem.name != name) continue;
        status(problem.message);
        return false;
    }

    auto rebuilt = document::rebuildFromParameters(candidate);
    history_.commit(std::move(rebuilt.document),
                    existing == nullptr ? "Add parameter " + name : "Edit " + name);
    refresh();
    status(existing == nullptr ? "Added " + name + "." : "Updated " + name + ".");
    return true;
}

bool Controller::removeParameter(const std::string& name) {
    const auto& doc = history_.current();
    if (doc.parameter(name) == nullptr) return false;

    auto rebuilt = document::rebuildFromParameters(doc.withoutParameter(name));
    history_.commit(std::move(rebuilt.document), "Delete parameter " + name);
    refresh();

    // Said plainly, because deleting a parameter something uses is a real consequence and the
    // feature going red is the only other signal.
    if (!rebuilt.problems.empty()) {
        status("Removed " + name + ", but " + std::to_string(rebuilt.problems.size())
               + " value(s) still refer to it.");
    } else {
        status("Removed " + name + ".");
    }
    return true;
}

}  // namespace cad::app
