# core/sketch — 2D sketch geometry, constraints, and the solve.
#
# The one module that links planegcs. Kept behind this boundary deliberately: GCS.h pulls in Eigen
# and Boost.Graph, and nothing above core/sketch should compile against either.
cad_core_library(cad_sketch
  src/Sketch.cpp
  src/Dxf.cpp
  src/DxfExport.cpp
  src/Infer.cpp
)

target_include_directories(cad_sketch PUBLIC include)
target_link_libraries(cad_sketch PUBLIC cad::kernel cad::log)

# PRIVATE: planegcs is an implementation detail. cad/sketch/Sketch.h exposes plain doubles and our
# own enums, never a GCS type, so a consumer of this module never needs the solver's headers -- and
# the day the solver is replaced, nothing above has to change.
target_link_libraries(cad_sketch PRIVATE cad::planegcs)

# DXF via dime (BSD-3-Clause), PRIVATE: dime's types never appear in cad/sketch/Dxf.h, so the
# reader could be replaced without touching a consumer. Chosen over libdxfrw/dxflib because those
# are GPL-2.0 and would relicense the whole product -- see COPYRIGHT.md.
find_package(dime CONFIG REQUIRED)
target_link_libraries(cad_sketch PRIVATE dime::dime)

target_include_directories(cad_sketch PRIVATE ${OpenCASCADE_INCLUDE_DIR})
target_link_libraries(cad_sketch PRIVATE TKernel TKMath TKG2d TKG3d TKGeomBase TKBRep TKTopAlgo)

# The Rust DXF reader, when the build has one.
#
# Optional rather than required because CAD_REQUIRE_RUST defaults to off: a contributor without a
# cargo toolchain must still be able to build and test the application. Both readers are compiled
# either way and both feed the same sketch-building pass, so the DXF acceptance tests are a real
# comparison between them rather than between two whole importers.
#
# CAD_USE_RUST_DXF exists so that comparison can actually be RUN. Configure a second build tree
# with it off and the same test suite exercises dime instead -- which is the only way to tell
# "the new reader agrees with the old one" from "the new reader agrees with the new tests".
# A switch that requires editing this file to flip is a switch nobody uses.
option(CAD_USE_RUST_DXF "Read DXF with the Rust parser when it is available" ON)

if(TARGET cad::parse AND CAD_USE_RUST_DXF)
  target_link_libraries(cad_sketch PRIVATE cad::parse)
  target_compile_definitions(cad_sketch PRIVATE CAD_HAVE_RUST_PARSE=1)
  message(STATUS "cad_sketch: DXF import uses the Rust parser")
else()
  target_compile_definitions(cad_sketch PRIVATE CAD_HAVE_RUST_PARSE=0)
  if(TARGET cad::parse)
    message(STATUS "cad_sketch: DXF import pinned to dime by CAD_USE_RUST_DXF=OFF")
  else()
    message(STATUS "cad_sketch: no Rust toolchain, DXF import falls back to dime")
  endif()
endif()
