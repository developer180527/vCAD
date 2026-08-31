#include "cad/document/Parameters.h"

#include "cad/document/Expressions.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <optional>

namespace cad::document {
namespace {

/// The dimension a parameter's stored value carries, so an expression that uses it type-checks
/// against the right thing.
std::optional<expr::Value> valueOf(const Property& parameter) {
    switch (typeOf(parameter.value)) {
        case PropertyType::Length:
            return expr::Value{std::get<units::Length>(parameter.value).base(), expr::kLength,
                               false};
        case PropertyType::Angle:
            return expr::Value{std::get<units::Angle>(parameter.value).base(), expr::kAngle, false};
        case PropertyType::Real:
            return expr::Value{std::get<double>(parameter.value), expr::kScalar, false};
        case PropertyType::Int:
            return expr::Value{static_cast<double>(std::get<std::int64_t>(parameter.value)),
                               expr::kScalar, false};
        default:
            return std::nullopt;
    }
}

}   // namespace

expr::Resolver ParameterValues::resolver() const {
    return [values = values](std::string_view name) -> std::optional<expr::Value> {
        const auto found = values.find(name);
        if (found == values.end()) return std::nullopt;
        return found->second;
    };
}

ParameterValues resolveParameters(const Document& doc) {
    ParameterValues out;

    // The graph. Only edges BETWEEN parameters matter: a reference to a name that is not a
    // parameter is not a dependency, it is an error, and it is reported when the value is needed.
    std::unordered_map<std::string, const Property*> byName;
    for (const auto& parameter : doc.parameters()) byName.emplace(parameter.name, &parameter);

    std::unordered_map<std::string, std::vector<std::string>> dependencies;
    std::unordered_map<std::string, std::vector<std::string>> dependents;
    std::unordered_map<std::string, std::size_t> remaining;

    for (const auto& parameter : doc.parameters()) {
        std::vector<std::string> referenced;
        if (!parameter.expression.empty()) {
            const auto names = expr::referencedNames(parameter.expression);
            if (names.ok()) referenced = names.value();
        }

        std::vector<std::string> deps;
        for (const auto& name : referenced) {
            if (byName.count(name) != 0 && name != parameter.name) deps.push_back(name);
        }

        // A parameter naming ITSELF is the shortest cycle there is, and it is the one Kahn cannot
        // describe: with the self-edge excluded above it looks like an ordinary node. Caught here
        // so the message can name the actual mistake.
        if (std::find(referenced.begin(), referenced.end(), parameter.name) != referenced.end()) {
            out.problems.push_back(
                ParameterProblem{parameter.name,
                                 "'" + parameter.name + "' is defined in terms of itself."});
            deps.clear();
        }

        remaining[parameter.name] = deps.size();
        for (const auto& dep : deps) dependents[dep].push_back(parameter.name);
        dependencies[parameter.name] = std::move(deps);
    }

    const auto selfReferential = [&](const std::string& name) {
        return std::any_of(out.problems.begin(), out.problems.end(),
                           [&](const ParameterProblem& p) { return p.name == name; });
    };

    // Kahn's algorithm: repeatedly take a parameter whose dependencies are all settled. Iterative
    // and bounded by construction -- the reason a cycle cannot become a stack overflow.
    std::vector<std::string> ready;
    for (const auto& parameter : doc.parameters()) {
        if (remaining[parameter.name] == 0) ready.push_back(parameter.name);
    }

    std::unordered_map<std::string, std::string> failed;
    std::size_t settled = 0;

    while (!ready.empty()) {
        const std::string name = ready.back();
        ready.pop_back();
        ++settled;

        const auto* property = byName.at(name);
        const expr::Value declared =
            valueOf(*property).value_or(expr::Value{0.0, expr::kScalar, false});

        const auto fail = [&](std::string why) { failed.emplace(name, std::move(why)); };

        if (selfReferential(name)) {
            fail({});   // already reported, with a better message
        } else if (!valueOf(*property)) {
            fail("A parameter has to be a length, an angle or a number.");
        } else if (property->expression.empty()) {
            out.values.emplace(name, declared);
        } else if (std::any_of(dependencies[name].begin(), dependencies[name].end(),
                               [&](const std::string& dep) { return failed.count(dep) != 0; })) {
            fail("This depends on a parameter that can't be worked out.");
        } else {
            // Resolved against everything settled so far, which topological order guarantees is
            // everything this expression can legally see.
            const auto resolver = out.resolver();
            base::Result<expr::Value> value =
                expr::evaluate(property->expression, property->expressionUnits, resolver);
            if (!value.ok()) {
                fail(value.error().message);
            } else if (!value.value().dim.isScalar() && value.value().dim != declared.dim) {
                // A parameter keeps the KIND it was declared with. `wall = width * width` in a
                // length parameter is an area, and letting it through hands an area to every
                // feature that uses `wall`.
                fail("This parameter holds " + expr::toString(declared.dim)
                     + ", but that works out to " + expr::toString(value.value().dim) + ".");
            } else if (value.value().dim.isScalar() && !declared.dim.isScalar()) {
                // A bare result adopts the parameter's declared kind, exactly as a bare number
                // typed into a length field does.
                if (declared.dim == expr::kLength) {
                    const auto asLength = expr::evaluateLength(
                        property->expression, property->expressionUnits, resolver);
                    if (!asLength.ok()) fail(asLength.error().message);
                    else out.values.emplace(name, expr::Value{asLength.value().base(),
                                                              expr::kLength, false});
                } else {
                    const auto asAngle = expr::evaluateAngle(property->expression, resolver);
                    if (!asAngle.ok()) fail(asAngle.error().message);
                    else out.values.emplace(name, expr::Value{asAngle.value().base(),
                                                              expr::kAngle, false});
                }
            } else {
                out.values.emplace(name, value.value());
            }
        }

        for (const auto& dependent : dependents[name]) {
            if (--remaining[dependent] == 0) ready.push_back(dependent);
        }
    }

    // Whatever Kahn could not settle is in a cycle, or downstream of one.
    if (settled < doc.parameters().size()) {
        std::vector<std::string> stuck;
        for (const auto& parameter : doc.parameters()) {
            if (remaining[parameter.name] > 0) stuck.push_back(parameter.name);
        }

        // Walk from a stuck parameter through stuck dependencies until a name repeats. That
        // revisited name and everything after it IS a cycle -- concrete names the user can act on,
        // rather than "there is a cycle somewhere in your forty parameters".
        std::vector<std::string> path;
        std::string at = stuck.front();
        while (std::find(path.begin(), path.end(), at) == path.end()) {
            path.push_back(at);
            const auto& deps = dependencies[at];
            const auto next = std::find_if(deps.begin(), deps.end(), [&](const std::string& d) {
                return remaining[d] > 0;
            });
            if (next == deps.end()) break;
            at = *next;
        }
        const auto start = std::find(path.begin(), path.end(), at);
        std::vector<std::string> cycle(start, path.end());

        std::string loop;
        for (const auto& name : cycle) loop += name + " -> ";
        loop += cycle.empty() ? at : cycle.front();

        for (const auto& name : stuck) {
            const bool inCycle = std::find(cycle.begin(), cycle.end(), name) != cycle.end();
            failed.emplace(name, inCycle
                                     ? "These parameters depend on each other: " + loop + "."
                                     : "This depends on parameters that depend on each other ("
                                           + loop + ").");
        }
    }

    // Reported in the document's own parameter order, so the list the user sees and the list of
    // problems agree.
    for (const auto& parameter : doc.parameters()) {
        const auto found = failed.find(parameter.name);
        if (found != failed.end() && !found->second.empty()) {
            out.problems.push_back(ParameterProblem{parameter.name, found->second});
        }
    }
    return out;
}

Reevaluation rebuildFromParameters(const Document& source) {
    const auto parameters = resolveParameters(source);

    // Write the resolved numbers back into the parameters themselves before anything else reads
    // them. A derived parameter's stored value would otherwise stay at whatever it was declared
    // with -- usually zero -- so the saved file would claim `wall = 0mm` while every feature using
    // it was built from 5. Anything reading the document without an evaluator (a future thumbnail,
    // a diff, a colleague's older build) would believe the file.
    Document doc = source;
    for (const auto& parameter : source.parameters()) {
        if (parameter.expression.empty()) continue;
        const auto found = parameters.values.find(parameter.name);
        if (found == parameters.values.end()) continue;   // failed; keep the last good value

        Property updated = parameter;
        switch (typeOf(parameter.value)) {
            case PropertyType::Length:
                updated.value = units::Length::fromBase(found->second.magnitude);
                break;
            case PropertyType::Angle:
                updated.value = units::Angle::fromBase(found->second.magnitude);
                break;
            case PropertyType::Real: updated.value = found->second.magnitude; break;
            case PropertyType::Int:
                updated.value = static_cast<std::int64_t>(std::llround(found->second.magnitude));
                break;
            default: continue;
        }
        doc = doc.withParameter(std::move(updated));
    }

    return reevaluate(doc, parameters.resolver());
}

std::string parameterNameProblem(const Document& doc, std::string_view name) {
    if (name.empty()) return "Give the parameter a name.";
    if (!expr::isValidName(name)) {
        return "'" + std::string(name)
               + "' can't be a parameter name. Use letters, digits and underscores, and avoid the "
                 "built-in names like pi, sin and mm.";
    }
    if (doc.parameter(name) != nullptr) {
        return "There is already a parameter called '" + std::string(name) + "'.";
    }
    return {};
}

}  // namespace cad::document
