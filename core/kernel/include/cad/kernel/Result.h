#pragma once

/// Compatibility header. The definitions live in `cad/base/Result.h`.
///
/// `Result`, `Error` and `ErrorCode` moved to `core/base` so that reporting an error does not
/// require linking the B-rep kernel — see the note there. They are re-exported into `cad::kernel`
/// rather than renamed at ~200 call sites, because `kernel::Result` is correct in every one of
/// them: kernel operations really do return it.
///
/// New code outside the kernel should prefer `cad/base/Result.h` and `cad::base::Result`, which is
/// what makes the dependency visible in the include line rather than only in CMake.
#include "cad/base/Result.h"

namespace cad::kernel {

using base::Error;
using base::ErrorCode;
using base::toString;

template <class T>
using Result = base::Result<T>;

}  // namespace cad::kernel
