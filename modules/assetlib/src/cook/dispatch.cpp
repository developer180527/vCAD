// ── Execution mode selection ─────────────────────────────────────────────────
// Which way does this cook run: isolated child process, or in-process behind the
// exception net? That decision, and the in-process path itself, are all that is
// here — the child implementations are per-platform files, so neither is
// half-visible to the compiler that cannot build it.
#include "cook/dispatch.h"
#include "cook/dispatch_internal.h"
#include "cook/env.h"
#include "assetlib/cook_result_file.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <thread>

#if defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN
    #define NOMINMAX
    #include <windows.h>
#else
    #include <sys/resource.h>
    #include <sys/wait.h>
    #include <signal.h>
    #include <unistd.h>
#endif

namespace assetlib {

CookResult cookInProcess(ICooker& cooker, const CookContext& ctx) {
    // Exception net: cook() runs third-party parsers (Assimp, stb, json) on
    // corrupt files — a throw escaping a worker std::thread is
    // std::terminate for the whole host process (cooker audit: "Unwrapped
    // Worker Thread Exception Paths"). Convert to a per-asset failure.
    try {
        return cooker.cook(ctx);
    } catch (const std::exception& e) {
        return { .success=false,
                 .error=std::string("cooker threw: ") + e.what() };
    } catch (...) {
        return { .success=false, .error="cooker threw a non-std exception" };
    }
}

CookResult dispatchCook(const std::filesystem::path& workerExe,
                        ICooker& cooker, const CookContext& ctx,
                        const CancelFn& isCancelled) {
    // Never start work the caller has already given up on.
    if (isCancelled && isCancelled())
        return { .success=false, .cancelled=true, .error="cook cancelled" };
    if (!workerExe.empty())
        return cookInWorkerProcess(workerExe, cooker, ctx, isCancelled);
    return cookInProcess(cooker, ctx);
}

} // namespace assetlib
