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
target_link_libraries(cad_sketch PUBLIC cad::kernel)

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
