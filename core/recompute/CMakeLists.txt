cad_core_library(cad_recompute
  src/Engine.cpp
  src/Features.cpp
)

target_include_directories(cad_recompute PUBLIC include)
target_link_libraries(cad_recompute PUBLIC cad::document)
