# core/ — the product. No Qt, no bgfx, no Metal, no platform toolkits.
# This is what compiles for iPadOS, what the plugin C ABI is carved from, and what the
# headless test suite exercises. Enforced by tools/check_layering.py.

add_subdirectory(kernel)
add_subdirectory(naming)

# M2+
# add_subdirectory(units)
# add_subdirectory(document)
# add_subdirectory(recompute)
# add_subdirectory(io)
# add_subdirectory(sketch)
# add_subdirectory(features)

add_library(cad_core INTERFACE)
add_library(cad::core ALIAS cad_core)
target_link_libraries(cad_core INTERFACE cad::kernel cad::naming)
