# core/ — the product. No Qt, no bgfx, no Metal, no platform toolkits.
# This is what compiles for iPadOS, what the plugin C ABI is carved from, and what the
# headless test suite exercises. Enforced by tools/check_layering.py.

# log is added by the ROOT CMakeLists, before the vendored modules that use it.
add_subdirectory(kernel)
add_subdirectory(naming)
add_subdirectory(units)
add_subdirectory(document)
add_subdirectory(io)

# Sketch: 2D constraint solving, on planegcs (modules/planegcs).
add_subdirectory(sketch)

# After sketch: the Sketch feature computes through the solver.
add_subdirectory(recompute)
# add_subdirectory(features)

add_library(cad_core INTERFACE)
add_library(cad::core ALIAS cad_core)
target_link_libraries(cad_core INTERFACE cad::log cad::kernel cad::naming cad::units cad::document cad::recompute cad::io cad::sketch)
