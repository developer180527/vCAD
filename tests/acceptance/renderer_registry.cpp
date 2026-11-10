/// The renderer registry's rules (ADR 0012), where they can be checked without a GPU.
///
/// None of these brings a renderer up. What they pin is what happens when one CANNOT be: the user
/// asked for a renderer by name, and the answer has to be that renderer or an error that names it.
/// A quiet substitute is the failure this registry exists to make impossible, because someone
/// comparing two renderers on a machine where one silently became the other learns nothing.

#include "cad/app/Controller.h"
#include "cad/render/Renderer.h"

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>

using namespace cad;

TEST_CASE("the build has a renderer, and each one can be named", "[render][registry]") {
    const auto all = render::renderers();
    REQUIRE_FALSE(all.empty());

    std::set<std::string> ids;
    int last = 1 << 30;
    for (const auto& f : all) {
        INFO("renderer '" << f.id << "'");
        CHECK(std::string(f.id) != "");
        CHECK(std::string(f.label) != "");
        CHECK(f.create != nullptr);
        CHECK(ids.insert(f.id).second);   // an id names ONE renderer: it is what a setting stores
        CHECK(f.priority <= last);        // listed in the order they are tried
        last = f.priority;
    }
}

TEST_CASE("a renderer asked for by name is that one or an error", "[render][registry]") {
    render::RendererConfig config;
    config.offscreen = true;
    config.viewport = {64, 64, 1.0f};

    std::vector<render::RendererAttempt> attempts;
    auto r = render::createRenderer(config, "no-such-renderer", &attempts);

    // Not the best renderer this build has. Nothing at all, and a message with the name in it.
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().message.find("no-such-renderer") != std::string::npos);
    // ...which says what COULD have been asked for, so the mistake is fixable from the message.
    CHECK(r.error().detail.find(render::renderers().front().id) != std::string::npos);

    // One attempt, the one asked for. A second entry would be a fallback having been tried.
    REQUIRE(attempts.size() == 1);
    CHECK(attempts[0].id == "no-such-renderer");
    CHECK_FALSE(attempts[0].ok);
    CHECK_FALSE(attempts[0].reason.empty());
}

TEST_CASE("the controller refuses a renderer it was asked for and cannot have", "[render][registry]") {
    app::Controller controller;
    controller.setPreferredRenderer("no-such-renderer");

    auto r = controller.attachRenderer(64, 64);

    REQUIRE_FALSE(r.ok());
    CHECK(r.error().message.find("no-such-renderer") != std::string::npos);
    // And it is still the headless controller it was: not half attached to something else.
    CHECK_FALSE(controller.rendererAttached());
    CHECK(controller.rendererName() == "null");
    REQUIRE(controller.rendererAttempts().size() == 1);
    CHECK(controller.rendererAttempts()[0].id == "no-such-renderer");
}
