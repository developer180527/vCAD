cmake_minimum_required(VERSION 3.16)
project(assetlib VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Sources ───────────────────────────────────────────────────────────────────
# One concern per TU, grouped by layer. Directory = layer, file = concern; a
# reader asking "how does staleness work" or "where is the Windows spawn" should
# not have to grep a 700-line file to find out.
add_library(assetlib STATIC
    src/uuid.cpp
    src/dependency_graph.cpp
    src/file_watcher.cpp
    src/task_graph.cpp              # cost-weighted DAG scheduler

    # ── Asset database (SQLite) ─────────────────────────────────────────────
    src/registry/asset_names.cpp    # AssetType <-> name / extension
    src/registry/schema.cpp         # tables, migrations, user_version, open
    src/registry/records.cpp        # row mapping, CRUD, single-record queries
    src/registry/dependencies.cpp   # asset->asset edges + dep source hashes
    src/registry/scanner.cpp        # filesystem walk, move detection

    # ── On-disk asset formats ───────────────────────────────────────────────
    src/formats/mesh_asset.cpp
    src/formats/texture_asset.cpp
    src/formats/scene_asset.cpp
    src/formats/material_asset.cpp
    src/formats/shader_asset.cpp

    # ── Derived Data Cache ──────────────────────────────────────────────────
    src/ddc/hash.cpp                # BLAKE3 wrappers + cook-key composition
    src/ddc/fs_util.cpp             # read-only blob file ops, temp naming
    src/ddc/store.cpp               # two-tier content-addressed blob store
    src/ddc/gc.cpp                  # budget + LRU eviction of the local tier
    src/ddc/manifest.cpp            # cached-output record format

    # ── Cook pipeline ───────────────────────────────────────────────────────
    src/cook/pipeline.cpp           # per-record: resolve, place, commit
    src/cook/pipeline_batch.cpp     # cookAll / cookGraph: the task graph
    src/cook/key.cpp                # identity + staleness (DDC keys)
    src/cook/dispatch.cpp           # execution mode selection + in-process
    src/cook/result_file.cpp        # the worker's sidecar result protocol
    src/cook/worker_posix.cpp       # fork + setrlimit + exec, reap w/ deadline
    src/cook/worker_win32.cpp       # CreateProcess + job object memory cap
)

# ── BLAKE3 (vendored, C) ─────────────────────────────────────────────────────
# Content hash for the DDC — cryptographic-strength keys are what make a
# SHARED cache safe (an FNV collision on a shared store silently serves the
# wrong bytes to the whole studio). Portable kernel everywhere; NEON on ARM.
set(BLAKE3_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/blake3)
set(BLAKE3_SRCS
    ${BLAKE3_DIR}/blake3.c
    ${BLAKE3_DIR}/blake3_dispatch.c
    ${BLAKE3_DIR}/blake3_portable.c
)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
    # NEON kernel autodetects via blake3_impl.h on AArch64.
    list(APPEND BLAKE3_SRCS ${BLAKE3_DIR}/blake3_neon.c)
else()
    # x86 SIMD kernels not vendored — portable only (correct, just slower).
    # Defines must be identical on EVERY blake3 TU: MAX_SIMD_DEGREE is derived
    # from them in blake3_impl.h and sizes buffers shared across the TUs.
    set_source_files_properties(${BLAKE3_SRCS} PROPERTIES COMPILE_DEFINITIONS
        "BLAKE3_NO_SSE2;BLAKE3_NO_SSE41;BLAKE3_NO_AVX2;BLAKE3_NO_AVX512")
endif()
target_sources(assetlib PRIVATE ${BLAKE3_SRCS})
target_include_directories(assetlib PRIVATE ${BLAKE3_DIR})
# The hash runs over every source byte in the project on scan; at Debug -O0
# it throttles registry scans + DDC key computation. Vendored, deterministic,
# never debugged with the engine — keep it optimized in Debug builds.
if(CMAKE_C_COMPILER_ID MATCHES "Clang|AppleClang|GNU")
    set_source_files_properties(${BLAKE3_SRCS} PROPERTIES COMPILE_OPTIONS "-O2")
endif()

add_library(assetlib::core ALIAS assetlib)

target_include_directories(assetlib
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# ── SQLite ────────────────────────────────────────────────────────────────────
# When linking the system package use SQLite::SQLite3, NOT SQLite3::SQLite3.
# The latter is a compatibility alias only newer CMake's FindSQLite3 adds
# (Homebrew's CMake 4 has it; the 3.28 on Ubuntu 24.04 does not), so it builds
# on this Mac and fails to configure on a stock Linux box. SQLite::SQLite3 is
# canonical and has existed since CMake 3.14 — well under our 3.20 floor.
#
# System SQLite where one exists (macOS SDK, Linux libsqlite3-dev) — those
# platforms keep using exactly what they already were. Windows has no system
# SQLite, and CI failed at configure there with "Could NOT find SQLite3", so
# fall back to the vendored amalgamation (third_party/sqlite3). That also makes
# assetlib genuinely standalone: no external package needed to consume it.
find_package(SQLite3 QUIET)
if(SQLite3_FOUND)
    message(STATUS "assetlib: SQLite = system (${SQLite3_VERSION})")
    target_link_libraries(assetlib PUBLIC SQLite::SQLite3)
else()
    message(STATUS "assetlib: SQLite = vendored amalgamation "
                   "(no system SQLite found)")
    # Its own subdirectory so MSVC's /RTC1 can be stripped for that TU alone —
    # see the CMakeLists there.
    add_subdirectory(third_party/sqlite3)
    target_link_libraries(assetlib PUBLIC assetlib_sqlite3)
endif()

# ── Platform ──────────────────────────────────────────────────────────────────
if(APPLE)
    target_link_libraries(assetlib PRIVATE "-framework CoreServices")
endif()

# ── Strict warnings, zero engine deps ────────────────────────────────────────
target_compile_options(assetlib PRIVATE
    $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-Wall -Wextra -Wno-unused-parameter>
)

# ── Optional: tests ──────────────────────────────────────────────────────────
option(ASSETLIB_BUILD_TESTS "Build assetlib tests" OFF)
if(ASSETLIB_BUILD_TESTS)
    add_subdirectory(tests)
endif()
