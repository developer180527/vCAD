cad_core_library(cad_document
  src/PropertyValue.cpp
  src/Document.cpp
)

target_include_directories(cad_document PUBLIC include)
target_link_libraries(cad_document PUBLIC cad::kernel cad::naming cad::units)
