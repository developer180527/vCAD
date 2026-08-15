cad_core_library(cad_units
  src/Units.cpp
)

target_include_directories(cad_units PUBLIC include)
# Depends on cad::base for Result/Error, and on NOTHING else. No OCCT.
#
# It used to link cad::kernel for the same two types, which meant a module about millimetres and
# degrees pulled in the whole B-rep kernel to say "that unit is not recognised". Moving Result to
# core/base cost one include line and removed OCCT from this module's dependency closure entirely.
target_link_libraries(cad_units PUBLIC cad::base)
