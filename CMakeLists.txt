cmake_minimum_required(VERSION 3.24)

# vcpkg toolchain must be set before project(). Pass -DCMAKE_TOOLCHAIN_FILE=... or set
# VCPKG_ROOT in the environment; see docs/BUILD.md.
if(DEFINED ENV{VCPKG_ROOT} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
      CACHE STRING "vcpkg toolchain")
endif()

project(CAD
  VERSION 0.0.1
  DESCRIPTION "Cross-platform parametric CAD"
  LANGUAGES CXX C)

# Windows has no rpath. A DLL is found beside the executable or on PATH, and nowhere else -- so
# with CMake's default layout, cad_tests.exe lands in build/tests while planegcs.dll lands in
# build/modules/planegcs, and the test binary cannot start. That is not a CI problem to paper over
# with a PATH entry in a workflow: it is equally broken for anyone running ctest on Windows.
#
# WINDOWS ONLY, deliberately. On Unix a shared library is a LIBRARY artifact and stays where it
# is; only executables would move, which would relocate every path anyone has typed for a spike or
# a test binary for no benefit -- rpath already solves this there. Import libraries (.lib) are
# ARCHIVE artifacts and also stay put, which matters because tests-rs/cad-sys/build.rs looks for
# them in build/abi.
if(WIN32)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()

# OCCT 8.x requires C++17 minimum. We build at 20; do not lower this without checking
# core/kernel and core/naming, which use concepts and designated initializers.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Applies to THIRD-PARTY dependencies only. The LGPL relink obligation for Qt and planegcs
# is satisfied by dynamic linking — see docs/decisions/0001-qt-linking.md.
#
# Our own core modules are always STATIC regardless (see cad_core_library in
# cmake/CadLayering.cmake): OCCT is a static archive, and duplicating it across several
# shared libraries duplicates its global state.
option(BUILD_SHARED_LIBS "Build third-party dependencies as shared libraries" ON)

option(CAD_BUILD_TESTS      "Build tests"                         ON)
option(CAD_BUILD_SPIKES     "Build M0 de-risking spikes"          ON)
option(CAD_BUILD_RENDER     "Build renderer (M3)"                 OFF)
option(CAD_BUILD_SHELL_QT   "Build Qt desktop shell (M4)"         OFF)
option(CAD_BUILD_SHELL_IOS  "Build SwiftUI iPad shell"            OFF)
option(CAD_ENABLE_ASAN      "Enable AddressSanitizer"             OFF)
option(CAD_STRICT_LAYERING  "Fail the build on layering violations" ON)

# Rust components (rust/cad-parse). Still OFF by default, and the earlier note here said this
# would flip to ON the day core/ linked it. That day has come — the DXF reader is Rust — and the
# answer changed on the way, because dime stayed as a working fallback rather than being deleted.
# Flipping this to ON would make a contributor without a cargo toolchain unable to build an
# application that runs perfectly well without one.
#
# So the requirement moved rather than disappearing: CI configures with -DCAD_REQUIRE_RUST=ON on
# every platform. That is where it belongs. A developer may opt out of the Rust path; the project
# may not, and the difference between "the Rust integration works on Windows" and "cargo was
# missing so CMake quietly skipped it and everything fell back to dime" is invisible from a green
# test run unless something asserts it.
# The Rust target triple, when cross-compiling. Empty means "build for the host", which is right
# for a native build and wrong for every other kind: cargo does not read CMake's toolchain.
set(CAD_RUST_TARGET "" CACHE STRING "Rust target triple, empty for host")
if(IOS AND NOT CAD_RUST_TARGET)
  set(CAD_RUST_TARGET "aarch64-apple-ios" CACHE STRING "" FORCE)
endif()

option(CAD_REQUIRE_RUST     "Fail the build if the Rust toolchain is missing" OFF)

# assetlib: the content-addressed DDC (ADR 0004). Vendored in-tree — see
# modules/assetlib/VENDORED.md for provenance, the sync procedure, and why this is a copy
# rather than a submodule of the engine repository it comes from.
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CadWarnings)
include(CadLayering)
include(CadShaders)
include(CadRust)

# core/log first, before the vendored modules: it depends on nothing, and planegcs's Base/Console
# shim routes the solver's diagnostics into it. A target must exist before it is referenced.
# BEFORE any add_subdirectory that might link it. An IMPORTED target has to exist by the time a
# consuming CMakeLists is processed — `if(TARGET cad::parse)` in tests/ was silently false when
# this ran at the bottom of the file, so the boundary test compiled out and the suite reported
# "no tests matched" rather than a failure. A guard that is false for the wrong reason is worse
# than no guard.
cad_add_rust_library()

add_subdirectory(core/log)

add_subdirectory(modules/assetlib EXCLUDE_FROM_ALL)

# planegcs: FreeCAD's 2D constraint solver, LGPL-2.1-or-later, vendored unmodified.
# See modules/planegcs/VENDORED.md for provenance, the licence audit, and the sync procedure.
# Not EXCLUDE_FROM_ALL: it must build on every platform in CI from the day it lands, because a
# vendored dependency that only compiles on the machine that added it is a dependency nobody else
# can touch.
add_subdirectory(modules/planegcs)

if(CAD_ENABLE_ASAN AND NOT MSVC)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

# --- Dependencies ------------------------------------------------------------------
# OCCT 8.x is vcpkg-compatible and has first-class ARM64 support on macOS/Windows.
# Note the 7.8+ DataExchange toolkit names: TKDESTEP, not TKSTEP.
# No version argument: OpenCASCADEConfig.cmake writes an EXACT-only version file, so
# `find_package(OpenCASCADE 8.0)` rejects 8.0.1. Check the version ourselves instead.
find_package(OpenCASCADE REQUIRED)
# vcpkg's baseline now carries Eigen 5.0.1. Eigen's config uses SameMajorVersion
# compatibility, so asking for 3.4 hard-rejects a 5.x install. We don't depend on anything
# Eigen 3-specific yet; when core/sketch and the solver land, re-check the Eigen 3 -> 5
# migration notes before assuming API compatibility.
find_package(Eigen3 REQUIRED NO_MODULE)

if(OpenCASCADE_VERSION VERSION_LESS 8.0)
  message(FATAL_ERROR
    "OCCT ${OpenCASCADE_VERSION} is too old. We need 8.0+: Standard_Failure only inherits "
    "std::exception from 8.0 onward, which core/kernel/Guard.h relies on.")
endif()

message(STATUS "OCCT ${OpenCASCADE_VERSION} at ${OpenCASCADE_INCLUDE_DIR}")

# --- Layers ------------------------------------------------------------------------
# Strict, one-directional. core knows nothing about render/app/shell.
add_subdirectory(core)

add_subdirectory(render)

# abi/ is the application-facing facade over core AND render, so it sits above both —
# it is deliberately NOT inside core/, which may not depend on the renderer.
add_subdirectory(app)
add_subdirectory(abi)

add_subdirectory(bindings/python)

# BEFORE any subdirectory that registers a test. add_test() outside an enabled testing context is
# silently discarded -- not an error, just a suite that reports "No tests were found". proshell
# registers its boundary probe and is built in the Qt configuration, where CAD_BUILD_TESTS is off,
# so gating this on that option loses the one test that configuration has.
enable_testing()

add_subdirectory(shell_qt)

# Swift is enabled HERE and not in shell_ios/CMakeLists.txt: enable_language() affects the whole
# project, and a subdirectory turning on a language for everyone is the kind of action-at-a-distance
# that makes a configure failure impossible to place. Behind the option so no desktop build pays a
# compiler-detection step for a language it does not use.
if(CAD_BUILD_SHELL_IOS)
  enable_language(Swift)
  add_subdirectory(shell_ios)
endif()

if(CAD_BUILD_TESTS)
  find_package(Catch2 3 REQUIRED)
  add_subdirectory(tests)
endif()

if(CAD_BUILD_SPIKES)
  add_subdirectory(spikes)
endif()

cad_add_layering_check()
