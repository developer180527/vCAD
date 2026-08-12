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
option(CAD_ENABLE_ASAN      "Enable AddressSanitizer"             OFF)
option(CAD_STRICT_LAYERING  "Fail the build on layering violations" ON)

# assetlib: the content-addressed DDC (ADR 0004). Vendored in-tree — see
# modules/assetlib/VENDORED.md for provenance, the sync procedure, and why this is a copy
# rather than a submodule of the engine repository it comes from.
add_subdirectory(modules/assetlib EXCLUDE_FROM_ALL)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CadWarnings)
include(CadLayering)

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
add_subdirectory(abi)

add_subdirectory(bindings/python)

if(CAD_BUILD_TESTS)
  enable_testing()
  find_package(Catch2 3 REQUIRED)
  add_subdirectory(tests)
endif()

if(CAD_BUILD_SPIKES)
  add_subdirectory(spikes)
endif()

cad_add_layering_check()
