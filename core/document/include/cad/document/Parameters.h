#pragma once

// Turning a document's named parameters into values something can be evaluated against.
//
// A parameter may be defined in terms of others -- `wall = width / 8` -- so this is a small
// dependency graph, resolved in order, with cycles reported rather than followed.
//
// # Why cycles are a first-class result and not an assertion
//
// `a = b + 1` and `b = a + 1` is a mistake a user makes by typing two perfectly reasonable things
// in the wrong order. It is not a corrupt file and not a programming error. An evaluator that
// simply asks for the value it needs recurses until the stack runs out, and the application dies
// with no message and no saved work. So resolution is iterative over a graph built in advance, and
// a cycle comes back as text naming the parameters in it.
//
// # Why this is not part of expr::
//
// expr:: knows how to turn text into a number given a way to look names up. It deliberately has no
// idea where names come from -- that is what lets the same evaluator serve a document's parameters,
// a test's map of three values, and one day an assembly's overrides. Deciding what `width` means
// is this layer's job, and it needs the Document.

#include "cad/document/Document.h"
#include "cad/document/Expressions.h"
#include "cad/expr/Expression.h"

#include <map>
#include <string>
#include <vector>

namespace cad::document {

/// A parameter that could not be resolved, in terms the user can act on.
struct ParameterProblem {
    std::string name;
    std::string message;
};

/// Every parameter's value, plus whatever went wrong.
struct ParameterValues {
    std::map<std::string, expr::Value, std::less<>> values;
    std::vector<ParameterProblem> problems;

    /// A lookup over `values`, suitable for expr::evaluate and document::reevaluate.
    ///
    /// Captures by value, so it stays valid after this object goes away and cannot observe a later
    /// edit -- one resolution, one consistent set of numbers.
    [[nodiscard]] expr::Resolver resolver() const;
};

/// Resolves the document's parameters in dependency order.
///
/// A parameter that fails -- a cycle, a name that does not exist, a dimension mismatch -- is left
/// out of `values` and named in `problems`. Everything that does resolve still does: one bad
/// parameter must not blank out the other forty.
///
/// A parameter's STORED VALUE declares its kind. `wall` holding a Length says wall is a length,
/// which is what `width * width` is checked against and refused for. For a derived parameter the
/// stored number is also the last resolved one -- rebuildFromParameters writes it back -- so a
/// saved file states what its parameters actually are.
[[nodiscard]] ParameterValues resolveParameters(const Document&);

/// Resolves the parameters, writes the resolved numbers back into the parameters themselves, and
/// pushes the result through every expression-backed property.
///
/// The single call an editor makes after changing a parameter. It exists so that "what happens when
/// a parameter changes" has one answer in one place; the two halves being callable separately is
/// for tests and for callers that already have a resolver.
[[nodiscard]] Reevaluation rebuildFromParameters(const Document&);

/// Whether `name` may be added, given what the document already has. Rejects duplicates, names that
/// are not valid identifiers, and names that would shadow a built-in like `pi` or a unit like `in`.
/// The message is what to show the user.
[[nodiscard]] std::string parameterNameProblem(const Document&, std::string_view name);

}  // namespace cad::document
