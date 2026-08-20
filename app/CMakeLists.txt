# app/ — UI-agnostic application layer. Document, selection, commands, undo, scene.
#
# No Qt, no SwiftUI, no widgets of any kind: the iPad shell needs this same controller, and any
# rule that lives in MainWindow does not exist on iPad. Enforced by tools/check_layering.py.
cad_core_library(cad_app
  # Controller, split by concern. One class, eight files: it reached 2574 lines in one, which is
  # past the point where anyone can hold it in their head. See app/src/controller/.
  src/SketchDrawing.cpp
  src/SelectionRanking.cpp
  src/StrokeShape.cpp
  src/controller/Lifetime.cpp
  src/controller/View.cpp
  src/controller/Picking.cpp
  src/controller/SketchEnvironment.cpp
  src/controller/SketchDrawing.cpp
  src/controller/Features.cpp
  src/controller/Files.cpp
  src/controller/Commands.cpp
  src/Session.cpp
)
target_include_directories(cad_app PUBLIC include)
target_link_libraries(cad_app PUBLIC cad::core cad::render)
