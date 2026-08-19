#pragma once

/// What the platform running the suite is allowed to do.
///
/// # Why these are skips and not `#if`s
///
/// Some of this suite cannot run on iOS, and the reasons are the operating system's rather than
/// ours: a sandboxed app may not fork, may not `dlopen` code it did not ship signed, and cannot see
/// the developer's filesystem where the fixture corpus lives.
///
/// Compiling those tests out would make the device suite *quietly smaller*. It would report a
/// confident green over a hundred and sixty cases while having run rather fewer, and nobody reading
/// the output would know which. That is the same lying-green pattern this project keeps finding, so
/// the tests stay compiled and SKIP with a reason printed. "163 passed, 11 skipped, here is why" is
/// evidence; "all tests passed" over an unknown subset is not.
///
/// The checks are RUNTIME, not preprocessor, wherever a runtime answer is available. A fixture
/// directory that is missing on a Linux CI runner should skip for the same reason and say the same
/// thing, rather than being an iOS special case that happens to also cover it.

#include <filesystem>
#include <string>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace cad::testing {

/// Whether this process may spawn another copy of itself.
///
/// iOS forbids `fork`/`exec` outright for sandboxed apps. The determinism check that re-runs the
/// build in a subprocess — to prove two independent processes agree — has no equivalent there.
inline bool canSpawnSubprocesses() {
#if defined(CAD_PLATFORM_IOS)
    return false;
#else
    return true;
#endif
}

/// Whether this process may load a plugin from disk.
///
/// iOS allows `dlopen` only for code inside the app bundle, signed with the same identity. vCAD's
/// plugin model — a directory of third-party shared libraries discovered at runtime — is not
/// expressible there, which is a product fact and not only a test fact: the iPad shell has no
/// plugins by design (see docs/design/IPAD_UX.md).
inline bool canLoadPlugins() {
#if defined(CAD_PLATFORM_IOS)
    return false;
#else
    return true;
#endif
}

/// Whether the repository's fixture corpus is reachable.
///
/// Checked by looking, because that is the honest question. On a device the path baked in at compile
/// time names a directory on the developer's Mac, which the device cannot see; on a CI runner with a
/// shallow checkout it may equally be absent.
inline bool hasRepoFixtures() {
#if defined(CAD_REPO_ROOT)
    std::error_code ec;
    return std::filesystem::is_directory(std::string(CAD_REPO_ROOT), ec);
#else
    return false;
#endif
}

}  // namespace cad::testing
