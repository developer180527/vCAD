// Cross-process determinism.
//
// The in-process check catches most non-determinism, but not everything: ASLR, hash-seed
// randomisation, and any accidental dependence on allocation addresses only show up across
// processes. If names or content hashes differ between runs, the DDC's shared tier produces
// cross-machine misses and is worthless — so this is a real requirement, not a nicety.
//
// Implemented by re-executing this same test binary with CAD_EMIT_DIGEST set; see main.cpp.

#include "Model.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdio>
#include <cstdlib>   // _putenv_s on MSVC
#include <string>

// MSVC spells these with a leading underscore; the semantics are the same.
#if defined(_WIN32)
#  define CAD_POPEN  _popen
#  define CAD_PCLOSE _pclose
#else
#  define CAD_POPEN  popen
#  define CAD_PCLOSE pclose
#endif

namespace cadtest {
extern std::string g_argv0;      // set in main.cpp
std::string emitDigest();        // the payload both parent and child compute
}  // namespace cadtest

TEST_CASE("M1: naming is deterministic across processes", "[m1][naming]") {
    REQUIRE_FALSE(cadtest::g_argv0.empty());

    // cmd.exe has no `VAR=value command` prefix syntax, so set the variable in our own
    // environment and let the child inherit it. Works identically on POSIX.
#if defined(_WIN32)
    _putenv_s("CAD_EMIT_DIGEST", "1");
    const std::string cmd = "\"" + cadtest::g_argv0 + "\"";
#else
    const std::string cmd = "CAD_EMIT_DIGEST=1 \"" + cadtest::g_argv0 + "\" 2>/dev/null";
#endif

    std::string child;
    {
        FILE* pipe = CAD_POPEN(cmd.c_str(), "r");
        REQUIRE(pipe != nullptr);
        std::array<char, 512> buf{};
        while (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe) != nullptr) {
            child += buf.data();
        }
        const int rc = CAD_PCLOSE(pipe);
        INFO("child exit code " << rc);
        REQUIRE(rc == 0);
    }

#if defined(_WIN32)
    _putenv_s("CAD_EMIT_DIGEST", "");   // do not let the parent re-enter the emit path
#endif
    const std::string parent = cadtest::emitDigest();
    REQUIRE_FALSE(parent.empty());

    INFO("parent:\n" << parent << "\nchild:\n" << child);
    CHECK(parent == child);
}
