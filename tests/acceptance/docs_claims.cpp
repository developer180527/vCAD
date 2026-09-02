// Capability claims in the docs, asserted against the build.
//
// # Why this file exists
//
// A comment explaining WHY code is shaped a certain way stays true forever. A document asserting
// WHAT EXISTS rots the moment someone commits, and it rots silently — nothing fails, nothing warns,
// and the next person to read it is misled by a file whose whole purpose is to inform them.
//
// Both had happened here. Three design documents stated that OCCT's `TKHLR` was "already linked",
// used to argue that drawings were closer than they looked; nothing linked it anywhere in the
// build. `STATUS.md` listed revolve and hole among the operations vCAD did not have, months after
// both shipped with commands and tests. An audit that lies is worse than no audit, because it is
// trusted.
//
// The fix is the one `feature_reachability.cpp` already established for a different rot: turn the
// claim into an assertion. A capability claim that is checked cannot go stale — the build fails
// instead, on the commit that made it false, which is the only moment anyone can cheaply fix it.
//
// # Why a marker rather than prose
//
// The guarded list in STATUS.md is a comment marker, not the sentence a reader sees. Parsing the
// prose would mean the test breaks when someone rewords a paragraph — which trains people to
// weaken the test rather than update the fact. The marker is stable, the prose is free, and the
// two are meant to be edited together.

#include "cad/app/Controller.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::string contentsOf(const fs::path& file) {
    std::ifstream in(file);
    REQUIRE(in.good());
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

fs::path repoFile(const std::string& relative) { return fs::path(CAD_REPO_ROOT) / relative; }

/// The words on a `<!-- guarded:<name> a b c -->` line, or an empty vector if there is no such
/// marker. An empty result FAILS the test that asks for one rather than passing vacuously: a marker
/// someone deleted must not read as "nothing to check".
std::vector<std::string> guardedList(const std::string& document, const std::string& name) {
    const std::string opening = "<!-- guarded:" + name + " ";
    const auto start = document.find(opening);
    if (start == std::string::npos) return {};
    const auto from = start + opening.size();
    const auto end = document.find("-->", from);
    if (end == std::string::npos) return {};

    std::vector<std::string> words;
    std::istringstream line(document.substr(from, end - from));
    for (std::string word; line >> word;) words.push_back(word);
    return words;
}

bool commandExists(const cad::app::Controller& app, const std::string& id) {
    for (const auto& command : app.commands()) {
        if (command.id == id) return true;
    }
    return false;
}

}  // namespace

TEST_CASE("nothing STATUS.md calls missing is actually reachable", "[docs][guard]") {
    // The rot that happened: revolve and hole sat in this list long after both had commands. The
    // catalogue is the right thing to check against, because the catalogue IS reachability — both
    // shells build their tools from it, so a feature absent from it does not exist as far as a user
    // is concerned.
    const auto status = contentsOf(repoFile("docs/STATUS.md"));
    const auto missing = guardedList(status, "missing-features");
    REQUIRE_FALSE(missing.empty());   // the marker itself must still be there

    cad::app::Controller app;
    for (const auto& feature : missing) {
        INFO("STATUS.md lists '" << feature << "' as missing, but feature." << feature
                                 << " is in the command catalogue. Update the list and the prose "
                                    "around it.");
        CHECK_FALSE(commandExists(app, "feature." + feature));
    }
}

TEST_CASE("every command STATUS.md omits is one it does not claim is missing", "[docs][guard]") {
    // The other direction, and the one that catches the rot at the moment it is CREATED rather than
    // later. Shipping Pattern makes the test above fail, which is the point -- but only if the word
    // is still in the list. This half asserts the list has not simply been emptied to keep the
    // suite quiet.
    const auto status = contentsOf(repoFile("docs/STATUS.md"));
    const auto missing = guardedList(status, "missing-features");
    REQUIRE(missing.size() >= 5);   // a floor, not a count: an emptied marker is the failure mode
}

TEST_CASE("no document claims TKHLR is linked while it is not", "[docs][guard]") {
    // Three documents said it was, and used that to argue drawings were nearly within reach. The
    // link line is the only authority on this, so it is what gets read.
    //
    // Written as an equivalence rather than a one-way check: linking TKHLR should also fail this,
    // because at that point the three documents saying it is NOT linked have become the stale ones.
    // A guard that only fires in one direction just moves the rot.
    const auto cmake = contentsOf(repoFile("core/kernel/CMakeLists.txt"));
    const bool linked = cmake.find("TKHLR") != std::string::npos;

    for (const std::string doc : {"docs/design/FEATURE_AUDIT.md",
                                  "docs/design/COMPETITIVE_REVIEW.md",
                                  "docs/design/WIRING_AUDIT.md"}) {
        const auto text = contentsOf(repoFile(doc));
        const auto mention = text.find("TKHLR");
        if (mention == std::string::npos) continue;

        // The sentence around the mention, which is where the claim lives.
        const auto from = text.rfind('\n', mention);
        const auto to = text.find("\n\n", mention);
        const auto claim = text.substr(from == std::string::npos ? 0 : from,
                                       (to == std::string::npos ? text.size() : to) - from);

        const bool saysNotLinked = claim.find("**not** linked") != std::string::npos;
        INFO(doc << " says TKHLR is " << (saysNotLinked ? "NOT linked" : "linked")
                 << ", the build says it is " << (linked ? "linked" : "NOT linked"));
        CHECK(saysNotLinked == !linked);
    }
}
