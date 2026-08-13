cad_core_library(cad_recompute
  src/Engine.cpp
  src/Features.cpp
  src/DdcCache.cpp
)

target_include_directories(cad_recompute PUBLIC include)
# cad::sketch is PRIVATE: the Sketch feature needs the solver to compute, but nothing that consumes
# recompute needs sketch types. Intra-layer within core, which the layering rule permits -- the rule
# is about core not reaching UP to render/app/shell.
target_link_libraries(cad_recompute PUBLIC cad::document cad::io
                      PRIVATE assetlib::core cad::sketch)
