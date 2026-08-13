# core/sketch — 2D sketch geometry, constraints, and the solve.
#
# The one module that links planegcs. Kept behind this boundary deliberately: GCS.h pulls in Eigen
# and Boost.Graph, and nothing above core/sketch should compile against either.
cad_core_library(cad_sketch
  src/Sketch.cpp
)

target_include_directories(cad_sketch PUBLIC include)
target_link_libraries(cad_sketch PUBLIC cad::kernel)

# PRIVATE: planegcs is an implementation detail. cad/sketch/Sketch.h exposes plain doubles and our
# own enums, never a GCS type, so a consumer of this module never needs the solver's headers -- and
# the day the solver is replaced, nothing above has to change.
target_link_libraries(cad_sketch PRIVATE cad::planegcs)

target_include_directories(cad_sketch PRIVATE ${OpenCASCADE_INCLUDE_DIR})
target_link_libraries(cad_sketch PRIVATE TKernel TKMath TKG2d TKG3d TKGeomBase TKBRep TKTopAlgo)
