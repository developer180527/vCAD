#pragma once

#include "cad/kernel/Result.h"

#include <exception>
#include <string>
#include <type_traits>
#include <utility>

namespace cad::kernel {

/// Every single call into OCCT goes through here. No exceptions.
///
/// OCCT 8.x made `Standard_Failure` inherit `std::exception`, so one catch clause covers
/// OCCT throws, our throws, and std throws. Do NOT reintroduce the pre-8.0
/// `OCC_CATCH_SIGNALS` / `Standard_Failure`-only pattern you will find in older tutorials
/// and in FreeCAD — it is obsolete and it swallows the std::exception path.
///
/// Note this does not catch OS-level signals (SEGV from a genuinely corrupt shape). OCCT
/// can still take the process down on pathological input. That is a known residual risk;
/// the mitigation is the validity gate on ingest (see Healing.h), not signal handlers.
template <class Fn>
auto guard(const char* what, Fn&& fn) -> Result<decltype(fn())> {
    using T = decltype(fn());
    try {
        if constexpr (std::is_void_v<T>) {
            std::forward<Fn>(fn)();
            return Result<void>{};
        } else {
            return Result<T>{std::forward<Fn>(fn)()};
        }
    } catch (const std::exception& e) {
        return Error{ErrorCode::KernelException,
                     std::string("Geometry operation failed: ") + what,
                     e.what()};
    } catch (...) {
        return Error{ErrorCode::KernelException,
                     std::string("Geometry operation failed: ") + what,
                     "unknown non-std exception from OCCT"};
    }
}

}  // namespace cad::kernel
