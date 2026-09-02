// Every feature the engine can compute must be reachable, or exempt on purpose.
//
// # Why this test exists
//
// Three capabilities have now shipped complete, tested, and invisible: clicking in the viewport,
// exporting a file, and Revolve/Hole/Translate. Each was found by a person noticing, not by a test.
// The pattern is always the same — the geometry works, the tests pass, and nothing connects it to
// anything a user can press.
//
// Both shells build their tools from `Controller`'s command catalogue by design (that is what makes
// one rule serve Qt and SwiftUI both), so "is there a command" is the whole question. This walks the
// engine's registry and asks it for every feature type.
//
// # About the exemption list
//
// The point of the list is not to make the test pass. It is that skipping a feature has to be a
// DECISION someone wrote down, rather than something nobody got round to. Adding a name here should
// feel like more work than adding the command, and it should be argued in the comment beside it.

#include "cad/app/Controller.h"
#include "cad/features/Builtins.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

using cad::app::Controller;

namespace {

/// Feature types with no command, and the reason each is allowed not to have one.
///
/// A reason has to say why a COMMAND is the wrong shape for it — not that the work is pending.
/// "Not done yet" is what this test is for.
const std::map<std::string, std::string>& exempt() {
    static const std::map<std::string, std::string> kExempt{
        {"Import",
         "Needs a file dialog to choose the file, and dialogs belong to the shell -- `app/` must "
         "stay free of any toolkit so both shells can reuse it. Reached through File > Import, "
         "which is why `Controller::importFile` takes a path rather than being a command."},
        {"Plane",
         "The three origin planes are seeded into every document rather than created; there is "
         "nothing for a user to invoke. A user-creatable DATUM plane -- offset, angled, through "
         "three points -- is a different feature that does not exist yet, and when it arrives it "
         "gets a command and comes off this list."},
    };
    return kExempt;
}

/// The command id a feature type would have, by the convention every existing one follows.
std::string commandIdFor(const std::string& featureType) {
    std::string id = "feature.";
    for (const char c : featureType) {
        id.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return id;
}

bool catalogueHas(const Controller& app, const std::string& id) {
    return std::any_of(app.commands().begin(), app.commands().end(),
                       [&id](const cad::app::Command& c) { return c.id == id; });
}

}  // namespace

TEST_CASE("every computable feature has a command, or a stated reason not to", "[reachability]") {
    Controller app;
    const auto registry = cad::features::builtins();

    std::vector<std::string> unreachable;
    for (const auto& type : registry.names()) {
        if (exempt().count(type) != 0) continue;
        if (!catalogueHas(app, commandIdFor(type))) unreachable.push_back(type);
    }
    std::sort(unreachable.begin(), unreachable.end());

    if (!unreachable.empty()) {
        std::string message =
            "these feature types compute and no user can reach them, because both shells build "
            "their tools from the command catalogue:";
        for (const auto& type : unreachable) {
            message += "\n  - " + type + " (expected command id \"" + commandIdFor(type) + "\")";
        }
        message +=
            "\nAdd a command in Commands.cpp, or add the type to this test's exemption list with a "
            "reason why a command is the wrong shape for it.";
        FAIL(message);
    }
}

TEST_CASE("the exemption list does not outlive its reasons", "[reachability]") {
    // An exemption for a feature type that no longer exists is a comment pretending to be a rule,
    // and it would silently excuse a DIFFERENT feature that later took the same name.
    const auto registry = cad::features::builtins();
    const auto names = registry.names();

    for (const auto& [type, reason] : exempt()) {
        INFO("exempt feature type: " << type);
        CHECK(std::find(names.begin(), names.end(), type) != names.end());
        // A reason has to be a reason. An empty string, or a placeholder, defeats the point of
        // making the list expensive to add to.
        CHECK(reason.size() > 40);
    }
}

TEST_CASE("a command that offers itself can be invoked", "[reachability]") {
    // The other half of reachable: a command that is present but permanently disabled is no more
    // usable than a missing one. On an EMPTY document at least the primitives must be available,
    // and every command must report its own enablement without throwing.
    Controller app;
    bool sawEnabled = false;
    for (const auto& command : app.commands()) {
        INFO("command: " << command.id);
        REQUIRE(command.enabled);   // a null predicate would crash the shell building its ribbon
        REQUIRE(command.invoke);
        if (command.enabled(app.context())) sawEnabled = true;
    }
    CHECK(sawEnabled);
}

// ── the blind spot this guard had ──────────────────────────────────────────────────────
//
// Everything above walks the FEATURE REGISTRY, so it can only see capabilities that are features.
// Measure is a Controller query -- it changes nothing, so it has no FeatureType -- and it shipped
// complete, tested and invisible while this file reported green. That was the fifth capability to
// do so, and the guard built to stop it structurally could not.
//
// So the question widens from "does every feature have a command" to "does every command reach a
// button". That covers queries, edits, view commands and anything else added later, without
// needing a list of what kinds exist.
//
// Read from the Qt shell's source rather than from a running window: the ribbon is built across
// several tabs and environments, and a test that instantiated one would be testing Qt. What makes
// this sound is that both shells build their tools from the same catalogue, so a command missing
// from the source is missing from the UI.

#include <filesystem>
#include <fstream>
#include <sstream>

namespace {

/// Commands with no button, and the reason each is allowed not to have one.
const std::map<std::string, std::string>& exemptCommands() {
    static const std::map<std::string, std::string> kExempt{
        {"sketch.edit",
         "Reached from the browser's context menu on a sketch, which is where a user looks for it "
         "-- not from the ribbon."},
    };
    return kExempt;
}

std::string qtShellSource() {
    const auto path = std::filesystem::path(CAD_REPO_ROOT) / "shell_qt/src/MainWindow.cpp";
    std::ifstream in(path);
    REQUIRE(in.good());
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

}   // namespace

TEST_CASE("every command reaches a button", "[reachability][commands]") {
    Controller controller;
    const auto source = qtShellSource();

    std::vector<std::string> unreachable;
    for (const auto& command : controller.commands()) {
        if (exemptCommands().count(command.id) != 0) continue;
        // Quoted, so "feature.cut" does not match "feature.cut_something" and a partial name
        // cannot make an absent command look present.
        if (source.find('"' + command.id + '"') == std::string::npos) {
            unreachable.push_back(command.id);
        }
    }

    for (const auto& id : unreachable) {
        UNSCOPED_INFO("no button for command: " << id);
    }
    CHECK(unreachable.empty());
}

TEST_CASE("nothing is still marked planned once it exists", "[reachability][commands]") {
    // The other half. A capability can be reachable AND still be advertised as unbuilt, which is
    // how a button ends up next to a placeholder for the same thing. Measure was `planned(...)` in
    // the ribbon while the Controller could already answer.
    const auto source = qtShellSource();
    Controller controller;

    for (const auto& command : controller.commands()) {
        // The ribbon labels a placeholder with the same words as the command it stands in for.
        const std::string placeholder = "planned(tr(\"" + command.label + "\")";
        INFO(command.id << " is implemented but the shell still calls it planned");
        CHECK(source.find(placeholder) == std::string::npos);
    }
}
