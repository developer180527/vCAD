#pragma once

// Re-evaluating the expressions a document stores.
//
// A property remembers `width * 2` as text; this is what turns that text back into a number when
// `width` changes, when a file is opened, or when undo restores an older parameter value.
//
// # Why this is in document/ and not app/
//
// It is the other half of the storage decision. A format that keeps expressions but leaves
// re-evaluation to whoever happens to be driving means every caller -- the Qt shell, the iPad
// shell, a plugin, the file loader, a headless batch run -- writes its own version of "which
// properties are stale and in what order", and they will not agree. Storing the text and defining
// what the text MEANS belong together.
//
// # What it does not do
//
// It does not decide where names come from. The resolver is supplied, so the same pass serves the
// document's own parameter table, a table plus assembly-level overrides, or a test with three
// values in a map. It also does not recompute geometry: it updates property values and marks what
// changed, and the recompute engine does the rest through the normal dirty path.

#include "cad/document/Document.h"
#include "cad/expr/Expression.h"

#include <string>
#include <vector>

namespace cad::document {

/// One expression that could not be evaluated, in terms the user can act on.
struct ExpressionProblem {
    ObjectId object;
    std::string property;
    std::string expression;
    std::string message;   ///< already legible: "There is no parameter called 'widht'."
};

struct Reevaluation {
    Document document;
    std::size_t changed = 0;                   ///< properties whose value actually moved
    std::vector<ExpressionProblem> problems;
};

/// Re-evaluates every expression-backed property in the document.
///
/// A property whose expression fails keeps its last good value and its owner is marked Failed with
/// the message -- the model stays openable and the tree says which feature is broken. Discarding
/// the value instead would turn one bad reference into a part that silently rebuilds at zero.
///
/// Bare numbers inside each expression are read in the unit that expression was ENTERED in
/// (Property::expressionUnits), never in the current user's preference.
[[nodiscard]] Reevaluation reevaluate(const Document&, const expr::Resolver&);

}  // namespace cad::document
