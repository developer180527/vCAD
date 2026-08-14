#pragma once

#include "cad/kernel/Result.h"

#include <cmath>
#include <exception>
#include <string>
#include <type_traits>
#include <utility>

namespace cad::kernel {

/// True when `v` is a usable positive dimension: finite, and greater than zero.
///
/// Written as `v > 0.0 && std::isfinite(v)` rather than as a rejection test, because EVERY
/// comparison with NaN is false — so the natural-looking `if (v <= 0.0) reject;` accepts NaN and
/// hands it to OCCT. That guard was in five places in this kernel, and a NaN dimension therefore
/// built a "box" that OCCT reported as done and BRepCheck_Analyzer rejected: a feature marked
/// Clean carrying an invalid shape, which then propagated into every boolean and mesh downstream.
/// Found by the geometry torture suite; see tests-rs/cad-bench/tests/torture.rs.
///
/// Infinity is excluded too. OCCT will happily build with it and produce unbounded geometry whose
/// bounding box poisons zoom-to-fit and the render path's float32 conversion.
[[nodiscard]] inline bool isPositiveFinite(double v) noexcept {
    return v > 0.0 && std::isfinite(v);
}

/// True when `v` is finite. For quantities that may legitimately be zero or negative — an offset,
/// a translation — where the only unacceptable value is a non-finite one.
[[nodiscard]] inline bool isFinite(double v) noexcept { return std::isfinite(v); }

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
