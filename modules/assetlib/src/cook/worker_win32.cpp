// ── Isolated cook worker: Windows ────────────────────────────────────────────
// The counterpart of worker_posix.cpp. Two things differ materially: argument
// quoting has to satisfy CommandLineToArgvW (asset paths routinely contain
// spaces), and the memory cap is a JOB OBJECT applied while the child is still
// suspended — so unlike a child-side setrlimit it predates the worker's first
// instruction.
#if defined(_WIN32)

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


namespace {

// Quote one argument for a CreateProcess command line, per the rules
// CommandLineToArgvW parses back: backslashes are literal EXCEPT when they
// immediately precede a quote, where each must be doubled. Asset paths
// routinely contain spaces, so naive concatenation silently splits arguments
// and the worker receives garbage.
void appendQuoted(std::wstring& cmd, const std::wstring& arg) {
    if (!cmd.empty()) cmd.push_back(L' ');
    if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos) {
        cmd += arg;
        return;
    }
    cmd.push_back(L'"');
    for (size_t i = 0; i < arg.size(); ++i) {
        size_t slashes = 0;
        while (i < arg.size() && arg[i] == L'\\') { ++slashes; ++i; }
        if (i == arg.size()) {
            cmd.append(slashes * 2, L'\\');   // before the closing quote
            break;
        }
        if (arg[i] == L'"') {
            cmd.append(slashes * 2 + 1, L'\\');
            cmd.push_back(L'"');
        } else {
            cmd.append(slashes, L'\\');
            cmd.push_back(arg[i]);
        }
    }
    cmd.push_back(L'"');
}

std::string lastErrorText(DWORD err) {
    char* buf = nullptr;
    const DWORD n = ::FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
            | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, err, 0, (char*)&buf, 0, nullptr);
    std::string s = n && buf ? std::string(buf, n) : ("error " + std::to_string(err));
    if (buf) ::LocalFree(buf);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

struct Handle {
    HANDLE h = nullptr;
    ~Handle() { if (h) ::CloseHandle(h); }
    Handle() = default;
    explicit Handle(HANDLE x) : h(x) {}
    Handle(const Handle&)            = delete;
    Handle& operator=(const Handle&) = delete;
    explicit operator bool() const { return h != nullptr; }
};

} // namespace

// Windows out-of-process cooking. Same contract as the POSIX path: one asset
// per child, outcome via the sidecar result file, and no failure mode that can
// take the host down with it.
//
// Two things differ from POSIX by necessity:
//  - The memory cap is applied by the PARENT through a job object, not by the
//    child through setrlimit (Windows has no setrlimit). This is strictly
//    better: the limit is in force before the child runs its first
//    instruction, with no window where it could allocate freely.
//  - JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE guarantees the child dies with us even
//    if the host is killed outright — orphan protection POSIX doesn't give us
//    here, since a SIGKILLed parent never reaches its own kill() call.
CookResult cookInWorkerProcess(const std::filesystem::path& workerExe,
                               ICooker& cooker, const CookContext& ctx,
                               const CancelFn& isCancelled) {
    namespace fs = std::filesystem;
    const fs::path resultPath = ctx.outputPath.string() + ".result";
    std::error_code ec;
    fs::remove(resultPath, ec);

    const long capMb = taskMemCapMb(cooker, ctx);

    std::wstring cmd;
    appendQuoted(cmd, workerExe.wstring());
    appendQuoted(cmd, ctx.sourcePath.wstring());
    appendQuoted(cmd, ctx.outputPath.wstring());
    appendQuoted(cmd, resultPath.wstring());
    appendQuoted(cmd, std::to_wstring(capMb));

    // Job object carrying the memory cap. Created before the process so the
    // child can be assigned while still suspended.
    Handle job(::CreateJobObjectW(nullptr, nullptr));
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
        li.BasicLimitInformation.LimitFlags =
            JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (capMb > 0) {
            li.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_PROCESS_MEMORY;
            li.ProcessMemoryLimit = (SIZE_T)capMb << 20;
        }
        ::SetInformationJobObject(job.h, JobObjectExtendedLimitInformation,
                                  &li, sizeof(li));
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    // CREATE_SUSPENDED so the job (and its memory cap) is attached before the
    // child executes anything.
    std::wstring cmdMutable = cmd;   // CreateProcessW may write to this buffer
    if (!::CreateProcessW(nullptr, cmdMutable.data(), nullptr, nullptr, FALSE,
                          CREATE_SUSPENDED | CREATE_NO_WINDOW,
                          nullptr, nullptr, &si, &pi)) {
        return { .success=false,
                 .error="cannot spawn cook worker: "
                      + lastErrorText(::GetLastError()) };
    }
    Handle proc(pi.hProcess), thread(pi.hThread);

    if (job) ::AssignProcessToJobObject(job.h, proc.h);
    if (::ResumeThread(thread.h) == (DWORD)-1) {
        ::TerminateProcess(proc.h, 1);
        return { .success=false,
                 .error="cannot resume cook worker: "
                      + lastErrorText(::GetLastError()) };
    }

    const long timeoutSec = envLong("COOK_TASK_TIMEOUT_SEC", 3600);
    const auto deadline   = std::chrono::steady_clock::now()
                          + std::chrono::seconds(timeoutSec);
    bool timedOut = false, aborted = false;
    for (;;) {
        const DWORD w = ::WaitForSingleObject(proc.h, 20);
        if (w == WAIT_OBJECT_0) break;
        if (w == WAIT_FAILED) {
            ::TerminateProcess(proc.h, 1);
            ::WaitForSingleObject(proc.h, INFINITE);
            return { .success=false,
                     .error="cannot wait on cook worker: "
                          + lastErrorText(::GetLastError()) };
        }
        const bool cancel = isCancelled && isCancelled();
        if (cancel || std::chrono::steady_clock::now() >= deadline) {
            ::TerminateProcess(proc.h, 1);
            ::WaitForSingleObject(proc.h, INFINITE);   // never leave an orphan
            if (cancel) aborted  = true;
            else        timedOut = true;
            break;
        }
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

    DWORD code = 0;
    ::GetExitCodeProcess(proc.h, &code);
    // An unhandled SEH exception surfaces as the NTSTATUS itself, which always
    // has the top two bits set (0xC0000005 = access violation, 0xC000000D =
    // invalid parameter, 0x80000003 = breakpoint). That is this platform's
    // equivalent of WIFSIGNALED, and it must not be confused with a cooker
    // that merely returned a nonzero code.
    if ((code & 0xF0000000u) == 0xC0000000u || code == 0x80000003u) {
        cleanupResult();
        char buf[32];
        std::snprintf(buf, sizeof(buf), "0x%08lX", (unsigned long)code);
        return { .success=false,
                 .error=std::string("cook worker crashed: exception ") + buf };
    }

    return finishFromResultFile(
        resultPath, ctx, "exited (code " + std::to_string((long)code) + ")");
}

} // namespace assetlib

#endif  // _WIN32
