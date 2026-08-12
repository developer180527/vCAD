# shell_qt/ — the Qt desktop shell. The ONLY place Qt is allowed.
#
# Qt comes from the system (Homebrew on macOS), not vcpkg: qtbase alone is a multi-hour build
# with 34 default features and it would dominate every cold CI run. See ADR 0008.
find_package(Qt6 6.5 COMPONENTS Widgets QUIET
  HINTS /opt/homebrew/opt/qt /usr/local/opt/qt $ENV{QT_ROOT})

if(NOT Qt6_FOUND)
  message(STATUS "Qt6 Widgets not found — the desktop shell will not be built. "
                 "macOS: brew install qt. Or set QT_ROOT.")
  return()
endif()

message(STATUS "Qt shell: Qt ${Qt6_VERSION}")
if(Qt6_VERSION VERSION_GREATER_EQUAL 6.9)
  # ADR 0008: we ship against 6.8 LTS. Building on newer is fine and convenient, but an API
  # introduced after 6.8 will compile here and fail at release, so say so out loud.
  message(STATUS "  note: shipping target is Qt 6.8 LTS — avoid APIs newer than 6.8")
endif()

qt_standard_project_setup()

qt_add_executable(vcad
  src/main.cpp
  src/MainWindow.cpp
  src/Ribbon.cpp
  src/Theme.cpp
  src/Icons.cpp
  src/ViewportPlaceholder.cpp
)
target_link_libraries(vcad PRIVATE cad::app Qt6::Widgets)
cad_set_warnings(vcad)

set_target_properties(vcad PROPERTIES
  MACOSX_BUNDLE FALSE          # a plain binary for now; bundling comes with signing at M4
  OUTPUT_NAME vcad)
