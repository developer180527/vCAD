#pragma once

/// What this application is built from.
///
/// # Why this is shared rather than a string in each shell
///
/// An About box is a support tool. The first question asked of any bug report is "which version of
/// what", and the answer has to be the truth about the BINARY — the kernel it was linked against,
/// the solver it vendored, the ABI it offers plugins — not a number someone remembered to update.
///
/// So every value here is read from the thing it describes: OCCT's own version macro, the vendored
/// solver's recorded commit, the ABI's own constants. A shell adds only what it alone knows, such
/// as its UI toolkit, and displays the list.

#include <string>
#include <vector>

namespace cad::app {

/// One line of the About box: what it is, and which version of it.
struct AboutEntry {
    std::string name;
    std::string value;
};

/// The application, its kernel, its solver, and everything else worth naming in a bug report.
[[nodiscard]] std::vector<AboutEntry> about();

}   // namespace cad::app
