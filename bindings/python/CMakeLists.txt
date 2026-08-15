# pybind11 comes from the Python interpreter we are binding to — NOT from vcpkg.
#
# vcpkg's pybind11 port pulls in a full CPython build (and fails on arm64-osx via libb2),
# and a module built against a different interpreter than the user runs is useless anyway.
# Ask the interpreter where its pybind11 lives.
#
#   pip install pybind11
find_package(Python3 COMPONENTS Interpreter Development.Module QUIET)

if(Python3_FOUND)
  execute_process(
    COMMAND "${Python3_EXECUTABLE}" -m pybind11 --cmakedir
    OUTPUT_VARIABLE _pybind11_dir
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE _pybind11_result)
  if(_pybind11_result EQUAL 0)
    # TO_CMAKE_PATH is not optional here. On Windows the interpreter returns a native path
    # (C:\hostedtoolcache\...), and CMake parses the backslashes as escape sequences —
    # "Invalid character escape '\h'" at the find_package call, which reads like a CMake
    # bug rather than a path-separator problem.
    file(TO_CMAKE_PATH "${_pybind11_dir}" _pybind11_dir)
    find_package(pybind11 CONFIG QUIET PATHS "${_pybind11_dir}" NO_DEFAULT_PATH)
  endif()
endif()

if(NOT pybind11_FOUND)
  message(STATUS
    "pybind11 not found — Python bindings disabled. "
    "Install it with: pip install pybind11")
  return()
endif()

message(STATUS "Python bindings: ${Python3_EXECUTABLE} (pybind11 ${pybind11_VERSION})")

pybind11_add_module(_cad src/module.cpp)
target_link_libraries(_cad PRIVATE cad::core)
cad_set_warnings(_cad)

# Land the module next to the package so `PYTHONPATH=bindings/python python3 -c "import cad"`
# works from a plain build tree, with no install step.
set_target_properties(_cad PROPERTIES
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bindings/python/cad")

# Windows: put the DLLs the extension needs beside it.
#
# `import cad` failed with "DLL load failed while importing _cad" on both Windows architectures.
# The extension lands in the source tree so PYTHONPATH finds it, while planegcs.dll lands in the
# build tree -- and since Python 3.8 an extension module does NOT resolve its dependent DLLs
# through PATH, so no amount of environment fixing in a workflow would have helped. The supported
# routes are os.add_dll_directory() at import time or shipping the DLLs beside the module; the
# second is what a wheel would do anyway, and it keeps the package importable by anyone who sets
# PYTHONPATH rather than only by CI.
#
# TARGET_RUNTIME_DLLS resolves the set from the link graph, so it picks up planegcs today and
# anything else that becomes shared later without this line needing to know.
if(WIN32)
  add_custom_command(TARGET _cad POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_RUNTIME_DLLS:_cad>" "$<TARGET_FILE_DIR:_cad>"
    COMMAND_EXPAND_LISTS
    COMMENT "Copying dependent DLLs beside the Python extension")
endif()
