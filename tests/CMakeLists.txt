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
cad_set_warnings(cad_tests)

catch_discover_tests(cad_tests)

# M1 is done when this passes.
add_test(NAME m1_acceptance COMMAND cad_tests "[m1]")
