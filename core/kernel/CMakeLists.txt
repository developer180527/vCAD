cad_core_library(cad_kernel
  src/Result.cpp
  src/Shape.cpp
  src/Primitives.cpp
  src/Booleans.cpp
  src/Fillet.cpp
  src/Healing.cpp
  src/Transform.cpp
)

target_include_directories(cad_kernel PUBLIC include)

# OCCT 8.x toolkit names. Note the TKDE* prefix on DataExchange — renamed in 7.8.
# Anything you copy from a pre-7.8 example will reference TKSTEP/TKIGES and fail to link.
target_link_libraries(cad_kernel
  PUBLIC
    Eigen3::Eigen
  PRIVATE
    # Foundation
    TKernel TKMath
    # ModelingData
    TKG2d TKG3d TKGeomBase TKBRep
    # ModelingAlgorithms
    TKGeomAlgo TKTopAlgo TKPrim TKBO TKBool TKFillet TKOffset TKShHealing TKMesh
)

# OCCT headers are PRIVATE on purpose: TopoDS_Shape must never reach a core public header.
target_include_directories(cad_kernel PRIVATE ${OpenCASCADE_INCLUDE_DIR})
