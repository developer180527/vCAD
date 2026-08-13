#pragma once

// SHIM — not upstream. See ../../VENDORED.md.
//
// planegcs writes solver diagnostics through FreeCAD's console. Every call site is
// `Base::Console().log(someString.c_str())`, so the surface we have to satisfy is one method.
//
// Off by default, and deliberately so. The solver logs per-iteration detail — residuals, subsystem
// partitioning, redundancy analysis — and a sketch drag runs the solver on every mouse move. Left
// on, that is thousands of lines a second through stderr, which is slow enough to be mistaken for a
// slow solver.
//
// Set CAD_PLANEGCS_LOG=1 in the environment to see it. That is the diagnostic we will want the
// moment a sketch fails to converge and the question is "what did the solver actually do", and it
// costs one getenv per process rather than a rebuild.

#include "cad/log/Log.h"

#include <cstdio>
#include <cstdlib>
#include <type_traits>

namespace Base {

class ConsoleShim {
public:
    /// One argument: printed literally, NOT as a format string. Most call sites build the message
    /// themselves and pass `str.c_str()`, and a solver diagnostic containing a stray '%' must not
    /// be interpreted as a conversion — that reads adjacent stack as arguments.
    void log(const char* text) const {
        // Routed through the log's ThirdParty category rather than stderr. The environment variable
        // still works and still defaults off -- the solver logs per iteration and a sketch drag
        // solves on every mouse move -- but when it is on the output now lands in the same file as
        // everything else, timestamped and interleaved correctly with our own messages.
        if (text == nullptr || !enabled()) return;
        CAD_DEBUG(cad::log::Category::ThirdParty) << text;
    }

    /// Format plus arguments, which upstream also uses ("...Group %d, index %d...").
    ///
    /// The static_assert is the point: fprintf is variadic and unchecked, so passing a std::string
    /// where a %s is expected is undefined behaviour that usually prints garbage and sometimes
    /// crashes. Upstream passes only integers today. If a future sync passes anything else, this
    /// fails to COMPILE instead of misbehaving at runtime in a solver inner loop.
    template <class... Args>
    void log(const char* fmt, Args... args) const {
        static_assert((std::is_scalar_v<Args> && ...),
                      "Base::Console shim: printf-style logging accepts only scalar arguments. "
                      "An upstream sync has started passing something else — widen this shim "
                      "deliberately rather than relaxing the assert.");
        if (fmt != nullptr && enabled()) {
            // Suppressed locally: the format string comes from vendored code, not from us, so
            // -Wformat-nonliteral has nothing actionable to say here.
#if defined(__clang__) || defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wformat-nonliteral"
#endif
            std::fprintf(stderr, fmt, args...);
#if defined(__clang__) || defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif
        }
    }

    // Upstream's console has more levels than planegcs currently uses. Declared so that a sync
    // which starts calling them compiles — a missing method would be a build error at a moment
    // when the useful signal is the diagnostic itself, not the shim's incompleteness.
    template <class... Args>
    void warning(const char* fmt, Args... args) const { log(fmt, args...); }
    template <class... Args>
    void error(const char* fmt, Args... args) const { log(fmt, args...); }
    template <class... Args>
    void message(const char* fmt, Args... args) const { log(fmt, args...); }

private:
    static bool enabled() {
        // Read once: this is called from the solver's inner loops.
        static const bool on = std::getenv("CAD_PLANEGCS_LOG") != nullptr;
        return on;
    }
};

inline ConsoleShim Console() { return {}; }

}  // namespace Base
