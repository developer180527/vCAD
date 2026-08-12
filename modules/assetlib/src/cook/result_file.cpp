// ── The worker's sidecar result protocol, parse side ──────────────────────────
// A cook worker reports its outcome through a file, not stdout (cookers print
// freely). Framing and validation live in assetlib/cook_result_file.h, shared
// with the writer; this is the part that turns a validated body into a
// CookResult. Also the per-task memory cap, which both spawn paths need.
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

CookResult finishFromResultFile(const std::filesystem::path& resultPath,
                                const CookContext& ctx,
                                const std::string& exitDesc) {
    namespace fs = std::filesystem;
    auto cleanup = [&] { std::error_code e; fs::remove(resultPath, e); };

    std::ifstream f(resultPath, std::ios::binary);
    if (!f)
        return { .success=false,
                 .error="cook worker " + exitDesc + " without writing a result" };

    const std::string raw((std::istreambuf_iterator<char>(f)),
                           std::istreambuf_iterator<char>());
    f.close();

    // Validate the frame BEFORE reading a single field. `RESULT ok` is the first
    // body line, so a worker killed mid-write leaves a file that parses as a
    // clean success with its OUTPUT lines missing — and the asset commits
    // without its siblings. Refuse anything whose trailer does not agree with
    // its body; an incomplete result is a failed cook, not a successful one.
    std::string body, frameErr;
    if (!cookresult::unframe(raw, body, frameErr)) {
        cleanup();
        return { .success=false,
                 .error="cook worker " + exitDesc + ", but its result file is "
                        "unusable: " + frameErr };
    }

    std::string verdict, error, line;
    std::istringstream in(body);
    while (std::getline(in, line)) {
        if      (line.rfind("RESULT ", 0) == 0) verdict = line.substr(7);
        else if (line.rfind("ERROR ", 0)  == 0) error   = line.substr(6);
        else if (line.rfind("OUTPUT ", 0) == 0) {
            if (ctx.addOutput) ctx.addOutput(fs::path(line.substr(7)));
        }
        else if (line.rfind("DEP ", 0) == 0) {
            if (ctx.addDependency)
                ctx.addDependency(UUID::fromString(line.substr(4)));
        }
    }
    cleanup();

    if (verdict == "ok")   return { .success=true };
    if (verdict == "skip") return { .success=false, .skipped=true, .error=error };
    if (verdict == "fail") return { .success=false, .error=error };
    return { .success=false, .error="worker result file had no RESULT line" };
}

// The per-task memory cap in MB: 2x the cooker's estimate with a 1 GB floor
// (estimates are ceilings, not promises), or COOK_TASK_MEM_CAP_MB verbatim.
long taskMemCapMb(ICooker& cooker, const CookContext& ctx) {
    long capMb = envLong("COOK_TASK_MEM_CAP_MB", 0);
    if (capMb <= 0)
        capMb = std::max((long)((cooker.estimatePeakBytes(ctx) * 2) >> 20), 1024L);
    return capMb;
}

} // namespace assetlib
