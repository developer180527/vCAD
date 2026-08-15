cad_core_library(cad_recompute
  src/Engine.cpp
  src/DdcCache.cpp
)

target_include_directories(cad_recompute PUBLIC include)
# NO cad::sketch, and that is the point of core/features existing.
#
# The Sketch feature needs the solver to compute, so this module used to link it -- which meant the
# dependency graph, the dirty propagation, the caching and the rollback could not be reused by any
# application that does not have a 2D constraint solver. The engine knows what a feature type IS;
# it does not know that one of them is called Sketch.
#
# cad::io remains PUBLIC because the cache key folds in external file digests, which is an engine
# concern rather than a geometry one.
target_link_libraries(cad_recompute PUBLIC cad::document cad::io
                      PRIVATE assetlib::core)
