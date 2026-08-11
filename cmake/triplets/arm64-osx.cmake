# Mirrors vcpkg's default arm64-osx, plus release-only builds.
# See README.md in this directory for why.
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Darwin)
set(VCPKG_OSX_ARCHITECTURES arm64)

# The one line that matters: skip the debug build of every dependency.
set(VCPKG_BUILD_TYPE release)
