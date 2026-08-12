# ── assetlib_sqlite3 — the vendored SQLite amalgamation ──────────────────────
# Its OWN CMakeLists purely for FLAG SCOPING: MSVC's Debug config carries /RTC1
# (runtime checks), and `/RTC1 /O2` is a hard command-line error (D8016). The
# flag lives in CMAKE_C_FLAGS_DEBUG, which is directory-scoped, so stripping it
# here affects this one 9.5 MB third-party TU and nothing else — assetlib's own
# sources keep their Debug checks.
#
# Nobody steps through SQLite with the engine, and an unoptimized amalgamation
# makes every registry scan and DDC lookup crawl in Debug, so it gets the same
# "vendored hot path always builds optimized" policy as the BC codecs.

if(MSVC)
    string(REPLACE "/RTC1" "" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
    string(REPLACE "/Od"   "" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
endif()

add_library(assetlib_sqlite3 STATIC sqlite3.c)
target_include_directories(assetlib_sqlite3 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# THREADSAFE=1 is REQUIRED, not a tuning knob: the registry runs one writer
# connection (the cook thread) alongside reader connections on other threads,
# which is only defined behaviour in serialized mode.
target_compile_definitions(assetlib_sqlite3 PUBLIC
    SQLITE_THREADSAFE=1
    SQLITE_OMIT_LOAD_EXTENSION=1     # nothing loads extensions
    SQLITE_DQS=0)                    # no double-quoted string literals

if(MSVC)
    target_compile_options(assetlib_sqlite3 PRIVATE /O2 /wd4996)
else()
    # -w: this is upstream code compiled as-is; its warnings are not ours to fix
    # and would drown the engine's own.
    target_compile_options(assetlib_sqlite3 PRIVATE -O2 -w)
endif()

if(UNIX)
    find_package(Threads REQUIRED)
    target_link_libraries(assetlib_sqlite3 PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
endif()
