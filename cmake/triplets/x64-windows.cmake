# Mirrors vcpkg's default x64-windows EXACTLY, plus release-only builds.
#
# Note VCPKG_LIBRARY_LINKAGE is *dynamic* here, unlike every other platform's default.
# Do not "tidy" that to static for consistency: it changes OCCT from DLLs to static archives
# on Windows only, which is a silent ABI change with no compile error to warn you.
# Verified against vcpkg triplets/x64-windows.cmake at the pinned baseline.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)
set(VCPKG_PROVIDED_FORTRAN ON)

# The one line that matters: skip the debug build of every dependency.
set(VCPKG_BUILD_TYPE release)
