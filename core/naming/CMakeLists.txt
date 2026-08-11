cad_core_library(cad_naming
  src/ElementName.cpp
  src/ElementMap.cpp
  src/NamingContext.cpp
)

target_include_directories(cad_naming PUBLIC include)
target_link_libraries(cad_naming PUBLIC cad::kernel)
target_include_directories(cad_naming PRIVATE ${OpenCASCADE_INCLUDE_DIR})
target_link_libraries(cad_naming PRIVATE TKernel TKMath TKBRep TKTopAlgo TKG3d)
