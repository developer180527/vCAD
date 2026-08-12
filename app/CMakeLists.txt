# app/ — UI-agnostic application layer. Document, selection, commands, undo, scene.
#
# No Qt, no SwiftUI, no widgets of any kind: the iPad shell needs this same controller, and any
# rule that lives in MainWindow does not exist on iPad. Enforced by tools/check_layering.py.
cad_core_library(cad_app
  src/Controller.cpp
  src/Session.cpp
)
target_include_directories(cad_app PUBLIC include)
target_link_libraries(cad_app PUBLIC cad::core cad::render)
