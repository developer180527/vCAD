// The claim in Guard.h, turned into a test.
//
// # What was wrong
//
// `Guard.h` said "every single call into OCCT goes through here. No exceptions." It was false for
// as long as it existed. `core/naming` reached OCCT on every operation -- mass properties for the
// sibling ordering, explorers, Modified() and Generated() -- and never called `guard` once, despite
// including its header. `Shape::measure` and `Shape::volume` called `BRepGProp` directly too, and
// `Controller::refresh` calls `volume()` on every rebuild.
//
// Nothing below them caught anything: neither shell installs a top-level handler, and nothing
// catches `Standard_Failure` outside `guard`. So the failure mode was not a wrong answer. It was a
// dead process, during an ordinary rebuild, with the user's unsaved work in it -- and the project
// has no autosave.
//
// # Why a test and not a fixed comment
//
// A safety claim in a header is worse than no claim, because people reason from it and write code
// assuming the boundary is sealed. Fixing the sentence would leave the next unguarded file to be
// found the same way this one was: by reading. This is the same move `docs_claims.cpp` makes for
// capability claims -- if it is asserted, assert it.
//
// # What this can and cannot see
//
// Per FILE, not per call. A file that touches OCCT must either call `guard` itself or be listed
// below with a reason. That granularity will not catch one unguarded function inside a file that
// guards elsewhere -- but it does catch the case that actually happened, which is a whole module
// reaching OCCT with nothing catching, and it catches the next new file that does.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

/// OCCT entry points that can throw `Standard_Failure`. Not exhaustive, and does not need to be:
/// these are the families this codebase actually calls.
const std::vector<std::string> kThrowingOcct = {
    "BRepGProp",     "BRepBuilderAPI", "BRepPrimAPI", "BRepAlgoAPI", "BRepCheck",
    "BRepAdaptor",   "BRep_Tool",      "BRepMesh",    "ShapeFix",    "ShapeUpgrade",
    "TopExp_Explorer",
};

/// Files that reach OCCT without calling `guard` themselves, each with the reason it is allowed.
///
/// Every entry here is a promise about reachability, and a promise is the thing this test cannot
/// check. Keep the list short and the reasons specific.
struct Exemption {
    std::string path;
    std::string why;
};

const std::vector<Exemption> kExempt = {
    {"core/naming/src/Measure.cpp",
     "internal:: only, and every caller reaches it through NamingContext's three guarded entry "
     "points (nameprimitive, propagate, nameCopy)."},

    // This was a KNOWN HOLE for one commit, and is no longer one: `contentHash` is guarded and
    // reports failure through `ShapeHash::valid`, which both caches now refuse to key on. It stays
    // exempt only because `unnamed` and `resolve` still reach OCCT without their own guard, and
    // they do not need one -- `unnamed` is called from nothing but NamingContext's three guarded
    // entry points, and `resolve` is a lookup in our own table plus an IsSame.
    {"core/naming/src/ElementMap.cpp",
     "contentHash guards itself; unnamed is reached only through NamingContext's guarded entry "
     "points. See shape_hash_validity.cpp for what a failed hash does."},
};

std::string contentsOf(const fs::path& file) {
    std::ifstream in(file);
    REQUIRE(in.good());
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

bool mentionsOcct(const std::string& source) {
    for (const auto& symbol : kThrowingOcct) {
        if (source.find(symbol) != std::string::npos) return true;
    }
    return false;
}

/// Every .cpp under the directories that are allowed to touch OCCT at all.
std::vector<fs::path> sourcesUnder(const std::vector<std::string>& roots) {
    std::vector<fs::path> found;
    for (const auto& root : roots) {
        const fs::path base = fs::path(CAD_REPO_ROOT) / root;
        if (!fs::exists(base)) continue;
        for (const auto& entry : fs::recursive_directory_iterator(base)) {
            if (entry.is_regular_file() && entry.path().extension() == ".cpp") {
                found.push_back(entry.path());
            }
        }
    }
    return found;
}

std::string relativeTo(const fs::path& file) {
    return fs::relative(file, fs::path(CAD_REPO_ROOT)).generic_string();
}

}  // namespace

TEST_CASE("every source that reaches OCCT guards it", "[guard][occt]") {
    std::set<std::string> exempt;
    for (const auto& e : kExempt) exempt.insert(e.path);

    std::size_t checked = 0;
    for (const auto& file : sourcesUnder({"core", "render", "abi"})) {
        const auto source = contentsOf(file);
        if (!mentionsOcct(source)) continue;

        const auto path = relativeTo(file);
        if (exempt.count(path) != 0) continue;

        ++checked;
        INFO(path << " calls into OCCT but never calls guard(). An OCCT throw from here reaches "
                     "no handler: neither shell installs one. Wrap the entry points, or add the "
                     "file to kExempt with the reason its callers are already guarded.");
        CHECK(source.find("guard(") != std::string::npos);
    }

    // A floor, so that a refactor which stops matching the OCCT symbol list cannot turn this into a
    // test that checks nothing and still passes.
    CHECK(checked >= 8);
}

TEST_CASE("the naming layer's entry points are the guarded ones", "[guard][occt]") {
    // Measure.cpp's exemption is a claim about reachability, and this is the half of it that can be
    // checked: the three entry points named in that reason must actually guard. If someone unwraps
    // one, the exemption silently stops being true and Measure.cpp goes back to being exposed.
    const auto naming = contentsOf(fs::path(CAD_REPO_ROOT) / "core/naming/src/NamingContext.cpp");

    for (const std::string entry : {"NamingContext::nameprimitive(", "NamingContext::propagate(",
                                    "NamingContext::nameCopy("}) {
        const auto at = naming.find(entry);
        INFO(entry << " not found in NamingContext.cpp");
        REQUIRE(at != std::string::npos);

        // The guarded wrapper is short; the unguarded body it calls is not. Looking just past the
        // signature keeps this from passing on a `guard(` that belongs to some later function.
        const auto window = naming.substr(at, 400);
        INFO(entry << " does not call guard() -- Measure.cpp's exemption rests on it doing so");
        CHECK(window.find("kernel::guard(") != std::string::npos);
    }
}

TEST_CASE("Guard.h no longer claims what is not true", "[guard][occt]") {
    // The sentence that made the hole invisible for as long as it was there.
    const auto header = contentsOf(fs::path(CAD_REPO_ROOT) / "core/kernel/include/cad/kernel/Guard.h");
    CHECK(header.find("No exceptions.") == std::string::npos);
}
