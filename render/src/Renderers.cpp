#include "cad/render/Renderer.h"

#include "cad/render/BgfxBackend.h"

#include <algorithm>
#include <array>
#include <cstdlib>

namespace cad::render {

using kernel::Error;
using kernel::ErrorCode;

namespace {

kernel::Result<std::unique_ptr<IRenderer>> createBgfx(const RendererConfig& config) {
    auto backend = std::make_unique<BgfxBackend>();
    if (auto r = backend->initialise(config); !r) return r.error();

    // Noop is not a failure to bgfx: it validates every call and draws nothing, so init succeeds
    // and the frame comes back blank with no error anywhere. Refused here, for every caller, unless
    // it is what was asked for. A viewport that silently renders nothing is the single most
    // expensive bug this project has had, twice.
    if (config.api != "noop" && backend->rendererName() == "Noop") {
        backend->shutdown();
        return Error{ErrorCode::Internal, "No GPU renderer available; the viewport would draw nothing.",
                     "bgfx selected the Noop renderer"};
    }
    return std::unique_ptr<IRenderer>(std::move(backend));
}

// Highest priority first. bgfx is the only entry today; librhi goes above it (ADR 0012).
constexpr std::array<RendererFactory, 1> kRenderers{{
    {"bgfx", "bgfx", 10, nullptr, createBgfx},
}};

const RendererFactory* find(std::string_view id) {
    const auto it = std::find_if(kRenderers.begin(), kRenderers.end(),
                                 [&](const RendererFactory& f) { return id == f.id; });
    return it == kRenderers.end() ? nullptr : &*it;
}

}  // namespace

std::span<const RendererFactory> renderers() { return kRenderers; }

kernel::Result<std::unique_ptr<IRenderer>> createRenderer(const RendererConfig& config,
                                                          std::string_view preferred,
                                                          std::vector<RendererAttempt>* attempts) {
    const auto record = [&](std::string_view id, bool ok, std::string reason) {
        if (attempts != nullptr) attempts->push_back({std::string(id), ok, std::move(reason)});
    };

    std::string asked(preferred);
    if (asked.empty()) {
        if (const char* env = std::getenv("CAD_RENDERER"); env != nullptr) asked = env;
    }

    // Asked for by name: that one, or an error. Never a quiet substitute.
    if (!asked.empty()) {
        const RendererFactory* f = find(asked);
        if (f == nullptr) {
            std::string known;
            for (const auto& r : kRenderers) known += std::string(known.empty() ? "" : ", ") + r.id;
            record(asked, false, "not in this build");
            return Error{ErrorCode::InvalidInput, "There is no renderer called '" + asked + "'.",
                         "this build has: " + known};
        }
        std::string why;
        if (f->available != nullptr && !f->available(&why)) {
            record(asked, false, why);
            return Error{ErrorCode::Unsupported,
                         "The '" + asked + "' renderer cannot run on this machine.", why};
        }
        auto r = f->create(config);
        record(asked, r.ok(), r ? std::string() : r.error().message + " (" + r.error().detail + ")");
        return r;
    }

    // Nothing asked for: the first that comes up, in priority order.
    std::string reasons;
    for (const RendererFactory& f : kRenderers) {
        std::string why;
        if (f.available != nullptr && !f.available(&why)) {
            record(f.id, false, why);
            reasons += std::string(f.id) + ": " + why + "\n";
            continue;
        }
        auto r = f.create(config);
        if (r) {
            record(f.id, true, {});
            return r;
        }
        const std::string reason = r.error().message + " (" + r.error().detail + ")";
        record(f.id, false, reason);
        reasons += std::string(f.id) + ": " + reason + "\n";
        // With one renderer its own error is the most useful thing to return, unwrapped.
        if (kRenderers.size() == 1) return r.error();
    }
    return Error{ErrorCode::Unsupported, "No renderer could start on this machine.", reasons};
}

}  // namespace cad::render
