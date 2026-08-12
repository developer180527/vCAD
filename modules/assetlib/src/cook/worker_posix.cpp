// ── Isolated cook worker: POSIX ──────────────────────────────────────────────
// One asset per child process. Exit-by-signal, an unusable result file, or a
// deadline overrun all become a per-asset failure — the host process (the
// EDITOR) never dies with a corrupt FBX.
//
// The whole file compiles to nothing on Windows; worker_win32.cpp is its
// counterpart. Both are listed unconditionally in CMake so that neither can rot
// unnoticed behind a platform the maintainer does not build.
#if !defined(_WIN32)

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


// One asset per child process. Exit-by-signal, a missing/garbled result file,
// or a deadline overrun all become a per-asset failure — the host process
// (editor!) never dies with it.
CookResult cookInWorkerProcess(const std::filesystem::path& workerExe,
                               ICooker& cooker, const CookContext& ctx,
                               const CancelFn& isCancelled) {
    namespace fs = std::filesystem;
    const fs::path resultPath = ctx.outputPath.string() + ".result";
    std::error_code ec;
    fs::remove(resultPath, ec);

    // Hard per-task memory cap, applied by the CHILD via setrlimit: a runaway
    // import hits ENOMEM/bad_alloc inside its own process instead of OOM-ing
    // the machine.
    const long capMb = taskMemCapMb(cooker, ctx);

    std::string exeArg = workerExe.string();
    std::string srcArg = ctx.sourcePath.string();
    std::string outArg = ctx.outputPath.string();
    std::string resArg = resultPath.string();
    std::string capArg = std::to_string(capMb);
    char* argv[] = { exeArg.data(), srcArg.data(), outArg.data(),
                     resArg.data(), capArg.data(), nullptr };

    // fork + setrlimit + exec, NOT posix_spawn. rlimits are inherited across
    // exec, so applying the cap in the child between fork and exec makes it
    // predate the worker's first instruction — including its static
    // initializers. Applying it inside the worker's own main() (from argv, which
    // is what this used to do, and still does as a belt-and-braces second
    // application) leaves everything before main uncapped. posix_spawn has no
    // rlimit attribute in POSIX, so there is no way to express this through it.
    // This gives POSIX the property the Windows path already had: it assigns the
    // job object while the process is still suspended.
    //
    // Between fork and exec, only async-signal-safe calls are legal. This parent
    // is multithreaded (the cook graph's worker pool), so a malloc here could
    // deadlock on a lock some other thread held at fork time. setrlimit and
    // execv are syscalls, and every string above was built before the fork.
    pid_t pid = ::fork();
    if (pid < 0)
        return { .success=false,
                 .error=std::string("cannot fork cook worker: ")
                        + std::strerror(errno) };
    if (pid == 0) {
        if (capMb > 0) {
            rlimit rl{ (rlim_t)capMb << 20, (rlim_t)capMb << 20 };
            ::setrlimit(RLIMIT_DATA, &rl);   // heap; the one that bites on macOS
            ::setrlimit(RLIMIT_AS,   &rl);   // address space; no-op on macOS
        }
        ::execv(exeArg.c_str(), argv);       // inherits environ, so the
                                             // fault-injection vars still reach it
        ::_exit(kExecFailedExit);            // exec returns only on failure
    }

    // Reap with a deadline. Default is generous — an HQ BC7 8K encode is
    // legitimately minutes — but bounded, so one wedged parse can't hold a
    // worker slot forever (COOK_TASK_TIMEOUT_SEC overrides).
    const long timeoutSec = envLong("COOK_TASK_TIMEOUT_SEC", 3600);
    const auto deadline   = std::chrono::steady_clock::now()
                          + std::chrono::seconds(timeoutSec);
    int  status    = 0;
    bool timedOut  = false;
    bool aborted   = false;
    bool reapError = false;
    for (;;) {
        const pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) break;
        if (r < 0) {
            // EINTR is a benign signal interruption of the call itself, NOT a
            // dead child — retrying is mandatory. Treating it as fatal both
            // misreported healthy cooks as crashes and leaked the child
            // (we'd leave the loop without ever reaping it).
            if (errno == EINTR) continue;
            reapError = true;
            break;
        }
        // Kill on cancellation (host shutting down) or deadline overrun. Both
        // reap the child so it can never outlive us as an orphan.
        const bool cancel = isCancelled && isCancelled();
        if (cancel || std::chrono::steady_clock::now() >= deadline) {
            kill(pid, SIGKILL);
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
            if (cancel) aborted  = true;
            else        timedOut = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    auto cleanupResult = [&] { std::error_code e; fs::remove(resultPath, e); };

    if (aborted) {
        cleanupResult();
        return { .success=false, .cancelled=true, .error="cook cancelled" };
    }
    if (timedOut) {
        cleanupResult();
        return { .success=false,
                 .error="cook timed out after " + std::to_string(timeoutSec)
                      + "s (worker killed)" };
    }
    if (reapError) {
        cleanupResult();
        return { .success=false,
                 .error=std::string("cannot reap cook worker: ")
                      + std::strerror(errno) };
    }
    if (WIFSIGNALED(status)) {
        cleanupResult();
        const int sig = WTERMSIG(status);
        return { .success=false,
                 .error=std::string("cook worker crashed: signal ")
                      + std::to_string(sig) + " (" + strsignal(sig) + ")" };
    }

    // exec never ran: a missing, non-executable, or wrong-architecture worker
    // binary. Distinguish it, because "cook worker exited (code 66) without
    // writing a result" sends you looking for a cooker bug instead of a build
    // or install problem.
    if (WIFEXITED(status) && WEXITSTATUS(status) == kExecFailedExit) {
        cleanupResult();
        return { .success=false,
                 .error="cannot exec cook worker at " + exeArg
                      + " (missing, not executable, or wrong architecture)" };
    }

    return finishFromResultFile(
        resultPath, ctx,
        "exited (code "
            + std::to_string(WIFEXITED(status) ? WEXITSTATUS(status) : -1)
            + ")");
}

} // namespace assetlib

#endif  // !_WIN32
