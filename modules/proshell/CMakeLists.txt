# proshell — the professional-application shell, independent of any domain.
#
# The ribbon, the theme, the marking menu and the icon machinery. Every one of these appears in
# SolidWorks, Inventor, Revit, Civil 3D, and in the architecture or city-planning application this
# is meant to be reused by. None of them knows what a solid is.
#
# The rule that keeps that true: THIS TARGET MAY NOT LINK cad::anything. Not core, not app, not the
# kernel. Qt and the standard library are the whole dependency set. A domain type reaching this
# library is not a style problem — it is the moment the library stops being reusable, and the link
# line is the only place that can be enforced rather than merely intended.
#
# Whether it holds is checked by `proshell_probe`, a second executable that links this and nothing
# else (see modules/proshell/probe). A boundary with one consumer is a boundary nobody has tested.

if(NOT TARGET Qt6::Widgets)
  message(STATUS "proshell: Qt6 Widgets not found — skipping")
  return()
endif()

# Headers are listed alongside the sources deliberately. AUTOMOC only scans files a target
# actually lists, so a Q_OBJECT class whose header lives in include/ and is not named here gets no
# meta-object and fails at LINK time with a missing staticMetaObject -- an error that says nothing
# about moc.
qt_add_library(proshell STATIC
  src/Ribbon.cpp
  src/Theme.cpp
  src/Icons.cpp
  src/MarkingMenu.cpp
  src/ShellWindow.cpp
  src/HomePage.cpp
  src/Settings.cpp
  src/SettingsWindow.cpp
  include/proshell/Ribbon.h
  include/proshell/Theme.h
  include/proshell/Icons.h
  include/proshell/MarkingMenu.h
  include/proshell/ShellWindow.h
  include/proshell/HomePage.h
  include/proshell/HomeModel.h
  include/proshell/Settings.h
  include/proshell/SettingsModel.h
  include/proshell/SettingsWindow.h
)

target_include_directories(proshell PUBLIC include)
target_link_libraries(proshell PUBLIC Qt6::Widgets)
cad_set_warnings(proshell)

add_library(proshell::proshell ALIAS proshell)

# The boundary test. See probe/main.cpp for why this is an executable rather than a comment.
#
# Linking proshell and Qt and nothing else is the ENTIRE assertion: if a domain type reaches this
# library, this target stops building. Registered with ctest in its --check form so it runs
# headless, where a link error is what we are actually looking for.
qt_add_executable(proshell_probe probe/main.cpp)
target_link_libraries(proshell_probe PRIVATE proshell::proshell Qt6::Widgets)
cad_set_warnings(proshell_probe)

add_test(NAME proshell_boundary COMMAND proshell_probe --check)
set_tests_properties(proshell_boundary PROPERTIES
  ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
  LABELS "shell")
