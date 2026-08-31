cad_core_library(cad_expr
  src/Expression.cpp
)

target_include_directories(cad_expr PUBLIC include)
# cad::units for the unit tables and the Quantity types, cad::base for Result. Nothing else -- an
# expression evaluator that could see the document would grow a way to reach into it, and then
# every field in the application would be able to run arbitrary lookups.
target_link_libraries(cad_expr PUBLIC cad::base cad::units)
