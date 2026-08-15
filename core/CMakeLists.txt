# core/ — the product. No Qt, no bgfx, no Metal, no platform toolkits.
# This is what compiles for iPadOS, what the plugin C ABI is carved from, and what the
# headless test suite exercises. Enforced by tools/check_layering.py.

# log is added by the ROOT CMakeLists, before the vendored modules that use it.
# base first: it is what everything else reports errors with, and it depends on nothing.
add_subdirectory(base)
add_subdirectory(kernel)
add_subdirectory(naming)
add_subdirectory(units)
add_subdirectory(document)
add_subdirectory(io)

# Sketch: 2D constraint solving, on planegcs (modules/planegcs).
add_subdirectory(sketch)

# After sketch: the Sketch feature computes through the solver.
add_subdirectory(recompute)
# The built-in feature catalogue, split OUT of recompute so the engine carries no geometry.
add_subdirectory(features)

add_library(cad_core INTERFACE)
add_library(cad::core ALIAS cad_core)
# The aggregate an application links. Note that cad::recompute and cad::features are SEPARATE
# entries: the engine and the built-in feature catalogue are different things now, and an
# application in another domain links the first without the second. This target is a convenience
# for vCAD, not the definition of the platform.
target_link_libraries(cad_core INTERFACE
  cad::base cad::log cad::kernel cad::naming cad::units
  cad::document cad::recompute cad::features cad::io cad::sketch)
