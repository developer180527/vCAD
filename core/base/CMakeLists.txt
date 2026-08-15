# core/base — the error and result types, and nothing else.
#
# No dependencies at all, on purpose. Everything reports errors, so anything this module needed
# would become a dependency of the entire codebase. It was carved out of core/kernel for exactly
# that reason: Result living there made cad_units link OCCT to report a bad unit string.
cad_core_library(cad_base
  src/Result.cpp
)
target_include_directories(cad_base PUBLIC include)
