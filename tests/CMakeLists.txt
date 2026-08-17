include(Catch)

add_executable(cad_tests
  acceptance/main.cpp
  acceptance/Model.cpp
  acceptance/m1_naming_stability.cpp
  acceptance/m1_determinism_subprocess.cpp
  acceptance/debug_dump.cpp
  acceptance/m1_units.cpp
  acceptance/m1_chamfer_healing.cpp
  acceptance/m2_external_input_cache.cpp
  acceptance/m2_abi_layout.cpp
)

target_include_directories(cad_tests PRIVATE acceptance
  ${CMAKE_SOURCE_DIR}/abi/include)
# Catch2::Catch2, not Catch2WithMain: acceptance/main.cpp owns main() so the
# cross-process determinism test can re-execute this binary.
target_link_libraries(cad_tests PRIVATE cad::core Catch2::Catch2)

# The Rust boundary test only exists when the Rust library does. Guarded rather than assumed
# because CAD_REQUIRE_RUST is OFF while the library is a stub: a contributor without a Rust
# toolchain still gets a green suite, minus this one file.
if(TARGET cad::parse)
  target_sources(cad_tests PRIVATE acceptance/rust_boundary.cpp)
  target_link_libraries(cad_tests PRIVATE cad::parse)
endif()

# The DXF differential compiles unconditionally -- it SKIPS at runtime when the build has no Rust
# reader, rather than vanishing from the suite. A test that disappears when a dependency is absent
# reports the same green as one that ran.
target_sources(cad_tests PRIVATE acceptance/dxf_differential.cpp)

# The manifest parser, reached by relative path into abi/src rather than through a public header.
# Deliberate: this is an internal detail of the loader, not part of the C ABI, and giving it a
# public header would invite a plugin to depend on the host's own parsing of its manifest.
target_sources(cad_tests PRIVATE acceptance/plugin_manifest.cpp acceptance/plugin_loader.cpp
                                 acceptance/plugin_museum.cpp
                                 acceptance/sketch_plane.cpp
                                 acceptance/face_pick.cpp
                                 acceptance/camera_align.cpp
                                 acceptance/selection.cpp
                                 acceptance/edge_highlight.cpp
                                 acceptance/tip_bodies.cpp
                                 acceptance/plugin_ribbon.cpp
                                 acceptance/plugin_load_entry.cpp
                                 acceptance/extrude_direction.cpp
                                 acceptance/camera_ray.cpp
                                 acceptance/sketch_point_at.cpp
                                 acceptance/navigation_gestures.cpp)
# cad::app for face_pick: picking is driven through Controller, because "a click at these pixels
# means that face" is a model rule that must exist on both shells, not a Qt one.
target_link_libraries(cad_tests PRIVATE cad::abi cad::app)
target_link_libraries(cad_tests PRIVATE cad::sketch)
# catch_discover_tests runs the suite from the build tree, so a relative path to a fixture finds
# nothing. Baked in rather than copied beside the binary: the fixture is also the Rust fuzzer's
# seed corpus, and two copies of a corpus drift.
target_compile_definitions(cad_tests PRIVATE CAD_REPO_ROOT="${CMAKE_SOURCE_DIR}")
cad_set_warnings(cad_tests)

add_subdirectory(plugins)
add_dependencies(cad_tests vcad_demo_plugin)
target_compile_definitions(cad_tests PRIVATE
  CAD_TEST_PLUGIN_DIR="${CMAKE_CURRENT_BINARY_DIR}/plugins")

catch_discover_tests(cad_tests)

# M1 is done when this passes.
add_test(NAME m1_acceptance COMMAND cad_tests "[m1]")

if(TARGET cad::parse)
  # Registered as its own ctest entry rather than folded into an existing tag: when the Rust
  # integration breaks it should be named in the failure list, not hidden inside a suite about
  # geometry.
  add_test(NAME rust_boundary COMMAND cad_tests "[rust]")
endif()

# Named separately for the same reason: "the two DXF readers disagree" is a specific and alarming
# finding, and it should say so in the failure list.
add_test(NAME dxf_differential COMMAND cad_tests "[differential]")
