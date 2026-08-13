cad_core_library(cad_io
  src/Serialize.cpp
  src/Format.cpp
  src/OcctProvider.cpp
  src/DocumentStore.cpp
)

target_include_directories(cad_io PUBLIC include)
target_link_libraries(cad_io PUBLIC cad::kernel cad::naming cad::units cad::document)
target_include_directories(cad_io PRIVATE ${OpenCASCADE_INCLUDE_DIR})

# SQLite backs the native document format (ADR 0003). PRIVATE on purpose: sqlite3.h must not
# reach consumers of cad_io. Which database the format uses is this module's business, and the day
# it changes nothing above has to be recompiled.
#
# vcpkg's sqlite3 port exports `unofficial::sqlite3::sqlite3`, NOT `SQLite::SQLite3` — that second
# name is CMake's own FindSQLite3 locating a SYSTEM copy. Linking that would build against
# whatever the platform ships, which is the accident that made assetlib work on macOS and would
# have failed on Windows, where there is no system SQLite at all.
find_package(unofficial-sqlite3 CONFIG REQUIRED)
target_link_libraries(cad_io PRIVATE unofficial::sqlite3::sqlite3)

# TKDE*-prefixed DataExchange toolkits — renamed in 7.8. TKSTEP will not link.
target_link_libraries(cad_io PRIVATE
  TKernel TKMath TKBRep TKG3d TKTopAlgo TKShHealing TKMesh
  TKXSBase TKDESTEP TKDEIGES TKDESTL)
