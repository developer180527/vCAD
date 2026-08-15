# Mirrors vcpkg's default arm64-windows EXACTLY, plus release-only builds.
#
# Dynamic library linkage, as on x64-windows and unlike every other platform's default. The note
# on x64-windows.cmake applies here for the same reason: changing it to static is a silent ABI
# change with no compile error to warn you.
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)

# The one line that matters: skip the debug build of every dependency.
set(VCPKG_BUILD_TYPE release)
