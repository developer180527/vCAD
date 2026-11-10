# libgui -- the interface library shell_libgui is built on (github.com/developer180527/libgui).
#
# PINNED to a commit, because libgui is pre-1.0 and changes its API most weeks. Moving the pin is a
# deliberate act: bump LIBGUI_PIN, build, run shell_libgui's tests, and note what moved.
#
# While co-developing the two, point at a local checkout instead and every edit there is picked up:
#
#     cmake -DFETCHCONTENT_SOURCE_DIR_LIBGUI=/path/to/libgui ...
#
# Built by libgui's own crates/libgui_c/CMakeLists.txt, which calls cargo and exports
# libgui::libgui (the static library, libgui.h and libgui.hpp). cargo must be on PATH.
include_guard(GLOBAL)
include(FetchContent)

set(LIBGUI_PIN 7d368585830d2cc99a14e2d7124a61383f996c54 CACHE STRING "libgui commit shell_libgui builds against")

FetchContent_Declare(libgui
  GIT_REPOSITORY https://github.com/developer180527/libgui.git
  GIT_TAG        ${LIBGUI_PIN}
  GIT_SHALLOW    FALSE          # a bare commit hash cannot be fetched shallowly from GitHub
  SOURCE_SUBDIR  crates/libgui_c)
FetchContent_MakeAvailable(libgui)

if(NOT TARGET libgui::libgui)
  message(FATAL_ERROR "libgui was fetched but did not define libgui::libgui -- its CMake changed; see ${libgui_SOURCE_DIR}")
endif()
# The bundled Inter, for embedding. libgui reads no files, and vCAD ships as a bare executable.
set(CAD_LIBGUI_FONT "${libgui_SOURCE_DIR}/assets/Inter.ttf" CACHE INTERNAL "")
