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

# The shell chrome now lives in modules/proshell, which knows nothing about CAD. What remains here
# is vCAD: the command catalogue, the CAD glyph vocabulary, the viewport and the sketch canvas.
add_subdirectory(${CMAKE_SOURCE_DIR}/modules/proshell ${CMAKE_BINARY_DIR}/modules/proshell)

qt_add_executable(vcad
  src/main.cpp
  src/MainWindow.cpp
  src/Icons.cpp
  src/Viewport.cpp
  src/SketchCanvas.cpp
  src/CadHomeModel.cpp
  src/PluginManager.cpp
)
# cad::abi for the plugin manager. The plugin system IS the ABI, so a UI over it talks to the
# ABI directly rather than through cad::app -- which also means the manager describes plugins with
# exactly the same code the loader uses to decide about them, and the two cannot disagree.
target_link_libraries(vcad PRIVATE cad::app cad::abi proshell::proshell Qt6::Widgets)
cad_set_warnings(vcad)

# The shader binaries have to sit beside the binary, because that is where main.cpp now tells
# the renderer to look. cad_shaders writes them to ${CMAKE_BINARY_DIR}/shaders, which is beside
# the SPIKES, not beside the shell.
#
# Driven by the shader FILES rather than by a POST_BUILD step on vcad. POST_BUILD only runs when vcad
# relinks, so a shader-only edit compiled into ${CMAKE_BINARY_DIR}/shaders and never arrived here --
# the app kept loading the previous binary, and the symptom was a shader change having no effect at
# all, which reads as the change being wrong rather than absent.
if(TARGET cad_shaders)
  get_target_property(_shader_outputs cad_shaders CAD_SHADER_OUTPUTS)
  set(_shader_stamp "${CMAKE_CURRENT_BINARY_DIR}/shaders.stamp")
  add_custom_command(
    OUTPUT "${_shader_stamp}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_BINARY_DIR}/shaders" "$<TARGET_FILE_DIR:vcad>/shaders"
    COMMAND ${CMAKE_COMMAND} -E touch "${_shader_stamp}"
    DEPENDS ${_shader_outputs}
    COMMENT "Copying shader binaries beside vcad")
  add_custom_target(vcad_shaders ALL DEPENDS "${_shader_stamp}")
  add_dependencies(vcad_shaders cad_shaders)
  add_dependencies(vcad vcad_shaders)
endif()

set_target_properties(vcad PROPERTIES
  MACOSX_BUNDLE FALSE          # a plain binary for now; bundling comes with signing at M4
  OUTPUT_NAME vcad)
