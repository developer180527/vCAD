/// What the About box says, and why it is generated rather than written.
///
/// An About box is a support tool: the first question asked of any bug report is "which version of
/// what", and a hard-coded answer is wrong the day after someone upgrades a dependency — which is
/// exactly when the number starts to matter. So every value is read from the thing it describes,
/// and this checks that it still is.

#include "cad/app/About.h"

#include <catch2/catch_test_macros.hpp>

#include <Standard_Version.hxx>

#include <algorithm>
#include <string>

using namespace cad;

namespace {

std::string valueOf(const std::vector<app::AboutEntry>& entries, const std::string& name) {
    const auto it = std::find_if(entries.begin(), entries.end(),
                                 [&](const app::AboutEntry& e) { return e.name == name; });
    return it == entries.end() ? std::string{} : it->value;
}

}   // namespace

TEST_CASE("About reports the kernel it was actually built against", "[about]") {
    // Read from OCCT's own header rather than typed here, so an upgrade cannot leave the box
    // claiming the old version. If this ever needs updating by hand, the mechanism has been lost.
    const auto entries = app::about();
    const auto kernel = valueOf(entries, "Geometry kernel");
    INFO("reported: " << kernel);
    CHECK(kernel.find(OCC_VERSION_COMPLETE) != std::string::npos);
}

TEST_CASE("every About entry says something", "[about]") {
    const auto entries = app::about();
    REQUIRE(entries.size() >= 5);
    for (const auto& entry : entries) {
        INFO(entry.name << " = " << entry.value);
        CHECK_FALSE(entry.name.empty());
        // An empty value is worse than an absent row: it reads as "unknown" for something the
        // build certainly knows.
        CHECK_FALSE(entry.value.empty());
    }
}

TEST_CASE("About names the solver and the plugin ABI", "[about]") {
    // The two a third party needs: which solver's behaviour they are seeing, and which ABI they
    // may compile a plugin against.
    const auto entries = app::about();
    CHECK_FALSE(valueOf(entries, "Sketch solver").empty());
    CHECK_FALSE(valueOf(entries, "Plugin ABI").empty());
}
