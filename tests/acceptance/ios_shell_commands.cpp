/// The iPad rail names real commands.
///
/// # Why this test exists
///
/// `shell_ios` reaches the shared layer by STRING: each rail button carries a command id like
/// `"feature.box"`, and `CadViewportView::runCommand:` looks it up in `Controller::commands()`. A
/// lookup that finds nothing does nothing — no crash, no message. The button simply stays greyed
/// out forever, because its enabled state comes from the same missing entry.
///
/// That is the exact shape of failure this project keeps re-learning: the shell agrees with itself.
/// Every id could be misspelled and the app would still build, install, launch and look correct.
/// Nothing in Swift, Objective-C or C++ would object, and the only symptom is a tool that is always
/// unavailable — which reads as "not implemented yet" rather than as a typo.
///
/// So the test reads the Swift source and asks the real catalogue about every id it finds. It is a
/// text scan, which is ugly, and it is the only thing here that can disagree: a compiler cannot
/// check a string against a table that is built at runtime in another language.
///
/// Skipped rather than failed where the repository is not reachable — on the iPad the compiled-in
/// source path names a directory on the developer's Mac. See Platform.h.

#include "Platform.h"

#include "cad/app/Controller.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>

using namespace cad;

namespace {

/// Every `command("…", "…", "some.id")` argument in the iPad shell's Swift sources.
///
/// Matched on the LAST string of the call rather than by parsing Swift: the third argument is the
/// id by construction, and a regex over three quoted strings is less likely to be wrong than a
/// hand-rolled parser of a language this project does not otherwise read.
std::set<std::string> idsReferencedBy(const std::filesystem::path& file) {
    std::ifstream in(file);
    std::stringstream buffer;
    buffer << in.rdbuf();
    const std::string text = buffer.str();

    std::set<std::string> ids;
    std::size_t at = 0;
    const std::string marker = "command(";
    while ((at = text.find(marker, at)) != std::string::npos) {
        at += marker.size();
        // Collect the quoted arguments up to the closing parenthesis of this call.
        std::vector<std::string> args;
        for (std::size_t i = at; i < text.size() && text[i] != ')'; ++i) {
            if (text[i] != '"') continue;
            const std::size_t end = text.find('"', i + 1);
            if (end == std::string::npos) break;
            args.push_back(text.substr(i + 1, end - i - 1));
            i = end;
        }
        // The THIRD argument, by position: `command(title, symbol, id)`.
        //
        // The first attempt took every dotted argument instead, on the theory that a command id is
        // dotted and a title is not. SF Symbol names are dotted as well — "square.on.square.dashed"
        // — so the test reported five failures that were entirely its own. Position is the actual
        // contract; a heuristic over the values is not.
        if (args.size() >= 3) ids.insert(args[2]);
    }
    return ids;
}

}   // namespace

TEST_CASE("every command the iPad shell names exists in the catalogue", "[ios][shell]") {
    if (!testing::hasRepoFixtures()) {
        SKIP("the repository sources are not reachable from this build");
    }

    const std::filesystem::path source =
        std::filesystem::path(CAD_REPO_ROOT) / "shell_ios" / "Sources" / "ProjectView.swift";
    REQUIRE(std::filesystem::exists(source));

    const auto referenced = idsReferencedBy(source);
    // A guard on the SCAN, not on the shell. If the extraction breaks — the call gets renamed, the
    // formatting changes — this test would otherwise pass by finding nothing to check, which is the
    // self-agreeing failure it was written to prevent.
    REQUIRE(referenced.size() >= 8);

    app::Controller controller;
    std::set<std::string> available;
    for (const auto& c : controller.commands()) available.insert(c.id);

    for (const auto& id : referenced) {
        INFO("shell_ios references command id: " << id);
        CHECK(available.count(id) == 1);
    }
}
