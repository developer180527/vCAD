# The C ABI. Built as a SHARED library on purpose: it is what Rust, the future SwiftUI
# shell, and native plugins load, and none of them can consume a C++ static archive.
add_library(cad_abi SHARED
  src/Session.cpp
)
add_library(cad::abi ALIAS cad_abi)

target_include_directories(cad_abi PUBLIC include)
target_link_libraries(cad_abi PRIVATE cad::core)
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
