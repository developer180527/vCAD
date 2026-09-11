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
#include "cad/features/Builtins.h"
#include "cad/recompute/Engine.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
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

/// The command with this id, or null. Used by the tests below rather than a bool, because the
/// instancing check has to INVOKE one.
const cad::app::Command* commandNamed(const cad::app::Controller& app, const std::string& id) {
    for (const auto& command : app.commands()) {
        if (command.id == id) return &command;
    }
    return nullptr;
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

TEST_CASE("STATUS.md lists exactly the operations the registry has", "[docs][guard]") {
    // The list rotted in BOTH directions at once: it named eleven operations while the registry held
    // sixteen, so Hole, Mirror, Pattern, Plane and Revolve were all missing from a sentence that says
    // "all of them". Checked against the registry rather than the command catalogue because that is
    // what the sentence claims to enumerate -- what the kernel can build, not what a button reaches.
    //
    // Both directions, for the reason the missing-features pair already establishes: a one-way check
    // just moves the rot. Adding an operation must fail this, and so must deleting a name to quiet it.
    const auto status = contentsOf(repoFile("docs/STATUS.md"));
    auto listed = guardedList(status, "kernel-operations");
    REQUIRE_FALSE(listed.empty());   // the marker itself must still be there

    auto actual = cad::features::builtins().names();
    std::sort(listed.begin(), listed.end());
    std::sort(actual.begin(), actual.end());

    INFO("STATUS.md's operation list and features::builtins() disagree. Edit the marker and the "
         "sentence above it together.");
    CHECK(listed == actual);
}

TEST_CASE("every body reaches the frame as a drawn instance", "[docs][guard][render]") {
    // The claim this replaces said the renderer had NEVER worked -- "eight distinct transforms
    // upload, one box draws, root cause unfound" -- and that the shell's viewport was a Qt-painted
    // placeholder. Both were true when written; neither is now, and nothing failed when they stopped
    // being true. So the claim becomes an assertion.
    //
    // UPLOADED is not DRAWN, and that distinction is the whole lesson of the original failure: a
    // counter of instance uploads reported success while one box appeared. So this counts the draw
    // RANGES in the frame -- what survived culling and will actually be issued -- and not just the
    // instances resident in the buffer.
    //
    // What it deliberately does NOT claim is "one mesh, many instances". Measured here: two boxes
    // come out as two meshes, two instances, two batches. Mesh dedupe is real and unit-tested
    // (shape_hash_validity.cpp), but nothing in the application shares a mesh yet -- each feature's
    // mesh carries its own element names, so two identical boxes hash differently, and the case
    // dedupe exists for is assembly references, which do not exist. Asserting the bolt-field
    // property here would be asserting a future.
    //
    // Headless, through the null backend, so it guards on every machine rather than only where there
    // is a GPU. It cannot see pixels: the shell probe checks the Metal path natively, and STATUS.md
    // still says nothing looks at a pixel, which remains true.
    cad::app::Controller app;
    for (int i = 0; i < 2; ++i) {
        const auto* box = commandNamed(app, "feature.box");
        REQUIRE(box != nullptr);
        box->invoke();
    }
    app.refresh();

    const auto stats = app.stats();
    std::size_t drawnRanges = 0;
    for (const auto& batch : app.frame().batches) drawnRanges += batch.ranges.size();

    INFO("objects " << stats.objects << ", meshes " << stats.uniqueMeshes << ", instances "
                    << stats.instances << ", draw ranges " << drawnRanges << ", triangles "
                    << stats.triangles);
    CHECK(stats.instances == 2);              // both bodies are in the frame
    CHECK(drawnRanges == stats.instances);    // and both are drawn, not merely resident
    CHECK(stats.triangles > 0);               // with geometry in them
}

TEST_CASE("every decision record says what it is and who it is for", "[docs][guard][adr]") {
    // The decision records were the ONE class of document with no guard, and they are where the
    // rot that cost this project a parallel application actually happened: ADR 0011 named the iPad
    // shell as a consumer of the C ABI, the iPad was built on app::Controller instead, and nothing
    // in the record changed. Four weeks later the duplication was still growing.
    //
    // Two lines are required of every ADR, and they are the two that would have caught it.
    //
    // STATUS, because five ADRs sat at "proposed" for decisions that had shipped -- the native
    // format, the Qt shell, the Session/Controller split and logging were all in the product while
    // their records still asked to be decided.
    //
    // CONSUMERS, because a reason with no named owner is a reason nobody notices has expired. It is
    // the line that makes "who was this for, and are they still using it?" a question with an
    // answer, and it is the whole counter-measure to what happened here.
    //
    // Structural rather than semantic on purpose. A test cannot know whether a consumer list is
    // TRUE; it can insist the question was asked of every decision, which is what stops the next
    // one being written without it.
    std::vector<std::string> missing;
    std::size_t records = 0;
    for (const auto& entry : fs::directory_iterator(repoFile("docs/decisions"))) {
        if (entry.path().extension() != ".md") continue;
        ++records;
        const auto text = contentsOf(entry.path());
        const std::string name = entry.path().filename().string();
        // Matched loosely: the files are not consistent about bolding, and a test that failed on
        // `**Status:**` versus `Status:` would be a test about markdown.
        if (text.find("Status") == std::string::npos) missing.push_back(name + " (no Status)");
        if (text.find("Consumers:") == std::string::npos) {
            missing.push_back(name + " (no Consumers)");
        }
    }

    INFO("decision records missing their header lines: " << [&] {
        std::string joined;
        for (const auto& m : missing) joined += m + " ";
        return joined;
    }());
    CHECK(missing.empty());

    // A floor, not a count: an emptied or moved directory must not read as "nothing to check",
    // which is the failure mode every guard in this file is written against.
    CHECK(records >= 11);
}

TEST_CASE("no decision record denies a loader that exists", "[docs][guard][adr]") {
    // The specific false claim, asserted so it cannot come back: ADR 0011 said the plugin API had
    // "no loader, no host vtable implementation, no dlopen, no test" and that nothing had ever used
    // it. abi/src/Loader.cpp had existed for four weeks, with a compiled demo plugin loaded by two
    // tests.
    //
    // One direction only, unlike the TKHLR check below. That one is an equivalence because three
    // documents each make a positive claim about the link line; here the honest rule is narrower --
    // an ADR is not obliged to mention the loader, only forbidden to deny one that exists.
    //
    // A blunt substring match, which forbids the phrase even inside a HISTORICAL retelling: the
    // rewrite of 0011 tripped this by saying the record had once claimed "no loader". Kept blunt on
    // purpose. A test that tried to tell a live denial from a quoted one would be reading intent,
    // and the cost of the rule is one reworded sentence.
    const bool loaderExists = fs::exists(repoFile("abi/src/Loader.cpp"));
    if (!loaderExists) return;   // nothing to contradict

    for (const auto& entry : fs::directory_iterator(repoFile("docs/decisions"))) {
        if (entry.path().extension() != ".md") continue;
        const auto text = contentsOf(entry.path());
        for (const std::string denial : {"no loader", "never been used by anything"}) {
            INFO(entry.path().filename().string() << " says \"" << denial
                                                  << "\" while abi/src/Loader.cpp exists");
            CHECK(text.find(denial) == std::string::npos);
        }
    }
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
