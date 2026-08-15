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
cad_set_warnings(cad_tests)

catch_discover_tests(cad_tests)

# M1 is done when this passes.
add_test(NAME m1_acceptance COMMAND cad_tests "[m1]")

if(TARGET cad::parse)
  # Registered as its own ctest entry rather than folded into an existing tag: when the Rust
  # integration breaks it should be named in the failure list, not hidden inside a suite about
  # geometry.
  add_test(NAME rust_boundary COMMAND cad_tests "[rust]")
endif()
