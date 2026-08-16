# The C ABI: the application-facing facade.
#
# Lives at the top level, NOT under core/, because it depends on core AND render. It sat in
# core/abi for two milestones and the layering check caught it the moment it needed to include
# a renderer header — the rule was right and the placement was wrong.
#
# SHARED on purpose: this is what Rust, the future SwiftUI shell and native plugins load, and
# none of them can consume a C++ static archive.
add_library(cad_abi SHARED
  src/Session.cpp
  src/Manifest.cpp
  src/Loader.cpp
)
add_library(cad::abi ALIAS cad_abi)

target_include_directories(cad_abi PUBLIC include)
target_link_libraries(cad_abi PRIVATE cad::core cad::render)
target_compile_definitions(cad_abi PRIVATE CAD_ABI_BUILD)
cad_set_warnings(cad_abi)

# Stable output name so the Rust build script can find it without guessing.
set_target_properties(cad_abi PROPERTIES
  OUTPUT_NAME cad_abi
  C_VISIBILITY_PRESET default
  CXX_VISIBILITY_PRESET default)

# macOS: bake an ABSOLUTE install name into the dylib.
#
# Cargo's `rustc-link-arg` only applies to the crate whose build script emits it, so the
# rpath cad-sys asks for never reaches cad-tests' test binaries. Rather than fight that with
# a wrapper for every consumer, the library states where it lives. This is a development
# convenience only — a shipped app relocates these to @rpath at packaging time, which is
# what macdeployqt does anyway.
if(APPLE)
  set_target_properties(cad_abi PROPERTIES
    BUILD_WITH_INSTALL_NAME_DIR ON
    INSTALL_NAME_DIR "${CMAKE_CURRENT_BINARY_DIR}")
endif()
