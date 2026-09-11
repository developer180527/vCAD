# cad::runtime — the document-computing stack, assembled once.
#
# Sits between render and app because it needs both core and render, and because BOTH consumers of
# it are above it: `app` (the shells) and `abi` (the C session the Rust suite drives). Each used to
# wire this stack by hand, which is how the two came to differ -- the shells had no disk cache tier
# at all. See include/cad/runtime/DocumentRuntime.h.
#
# NOT inside app/. `abi` would then have to link the whole application layer -- commands, picking,
# the sketch environment -- to share four members, and every plugin host and the iOS static library
# would carry it. `app` also already includes the ABI header for its version macros, so app and abi
# pointing at each other would be a cycle in the include graph even where the link graph has none.
add_library(cad_runtime STATIC
  src/DocumentRuntime.cpp
  include/cad/runtime/DocumentRuntime.h
)
add_library(cad::runtime ALIAS cad_runtime)

target_include_directories(cad_runtime PUBLIC include)
# PUBLIC: the header exposes recompute and render types by reference, so a consumer compiles
# against both.
target_link_libraries(cad_runtime PUBLIC cad::core cad::render)
cad_set_warnings(cad_runtime)
