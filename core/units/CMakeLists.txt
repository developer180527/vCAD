cad_core_library(cad_units
  src/Units.cpp
)

target_include_directories(cad_units PUBLIC include)
# Depends on kernel only for Result/Error. No OCCT.
target_link_libraries(cad_units PUBLIC cad::kernel)
