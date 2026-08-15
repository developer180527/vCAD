# core/features — vCAD's built-in feature types.
#
# The vocabulary, not the engine. This is where the geometry dependencies live: kernel, sketch, io.
# core/recompute deliberately has none of them, so a second application can reuse the dependency
# graph, the caching and the rollback without OCCT or the constraint solver.
cad_core_library(cad_features
  src/Features.cpp
)
target_include_directories(cad_features PUBLIC include)
target_link_libraries(cad_features PUBLIC cad::recompute)
target_link_libraries(cad_features PRIVATE cad::kernel cad::sketch cad::io)
