# app/ — UI-agnostic application layer. Document, selection, commands, undo, scene.
#
# No Qt, no SwiftUI, no widgets of any kind: the iPad shell needs this same controller, and any
# rule that lives in MainWindow does not exist on iPad. Enforced by tools/check_layering.py.
cad_core_library(cad_app
  # Controller, split by concern. One class, eight files: it reached 2574 lines in one, which is
  # past the point where anyone can hold it in their head. See app/src/controller/.
  src/SketchDrawing.cpp
  src/About.cpp
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
# About reports what this binary is BUILT FROM, so the facts come from the build rather than from
# literals in the source -- a version in two places is a version someone forgets to change.
target_include_directories(cad_app PRIVATE ${CMAKE_SOURCE_DIR}/abi/include
                                          ${OpenCASCADE_INCLUDE_DIR})
target_compile_definitions(cad_app PRIVATE
  CAD_VERSION="${PROJECT_VERSION}"
  CAD_BUILD_TYPE="$<IF:$<CONFIG:Debug>,Debug,Release>"
  CAD_PLANEGCS_COMMIT="64bc717")
target_link_libraries(cad_app PUBLIC cad::core cad::render)
