cad_core_library(cad_document
  src/PropertyValue.cpp
  src/Document.cpp
  src/Expressions.cpp
  src/Parameters.cpp
)

target_include_directories(cad_document PUBLIC include)
# cad::expr so that a stored expression can be turned back into a value here rather than in every
# shell. It brings only cad::base and cad::units with it -- no geometry, no document cycle.
target_link_libraries(cad_document PUBLIC cad::kernel cad::naming cad::units cad::expr)
