cad_core_library(cad_io
  src/Serialize.cpp
  src/Format.cpp
  src/OcctProvider.cpp
)

target_include_directories(cad_io PUBLIC include)
target_link_libraries(cad_io PUBLIC cad::kernel cad::naming cad::units cad::document)
target_include_directories(cad_io PRIVATE ${OpenCASCADE_INCLUDE_DIR})

# TKDE*-prefixed DataExchange toolkits — renamed in 7.8. TKSTEP will not link.
target_link_libraries(cad_io PRIVATE
  TKernel TKMath TKBRep TKG3d TKTopAlgo TKShHealing TKMesh
  TKXSBase TKDESTEP TKDEIGES TKDESTL)
