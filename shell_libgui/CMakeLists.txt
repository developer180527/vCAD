# shell_libgui/ -- vCAD's desktop shell on libgui, a PROTOTYPE beside shell_qt.
#
# Opt-in (CAD_BUILD_SHELL_LIBGUI) so no existing build changes. libgui leaves windows, input,
# clipboard and file dialogs to the application by design; the host provides them through SDL3,
# vcpkg's `shell-libgui` feature (configure with -DVCPKG_MANIFEST_FEATURES=shell-libgui). macOS only
# for now, because the renderer's native surface is only wired for Metal.
#
# The only places libgui may be included: src/gui/ (its plumbing) and the panels (src/*.cpp).
# The host, main.cpp, sees neither libgui nor bgfx. tools/check_layering.py keeps libgui out of
# every other layer.
if(NOT APPLE OR IOS)
  message(STATUS "shell_libgui: macOS only for now -- skipped")
  return()
endif()
if(NOT TARGET cad_render OR NOT TARGET cad_ui_shaders)
  message(FATAL_ERROR "shell_libgui needs the renderer and its shaders: configure with CAD_BUILD_RENDER=ON")
endif()

find_package(SDL3 CONFIG REQUIRED)
include(CadLibgui)
message(STATUS "shell_libgui: libgui ${LIBGUI_PIN}")

# The font, compiled in: libgui reads no files, and vCAD ships as a bare executable. Inter is under
# the SIL Open Font License, which permits embedding; COPYRIGHT.md must say so before this ships.
set(_font_cpp "${CMAKE_CURRENT_BINARY_DIR}/font_data.cpp")
file(READ "${CAD_LIBGUI_FONT}" _font_hex HEX)
string(LENGTH "${_font_hex}" _font_hex_len)
math(EXPR _font_size "${_font_hex_len} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _font_bytes "${_font_hex}")
file(WRITE "${_font_cpp}.in"
  "// Generated from ${CAD_LIBGUI_FONT}. Do not edit.\n"
  "extern \"C\" const unsigned char cad_libgui_font[] = {${_font_bytes}};\n"
  "extern \"C\" const unsigned long cad_libgui_font_size = ${_font_size};\n")
configure_file("${_font_cpp}.in" "${_font_cpp}" COPYONLY)

# The theme, compiled in for the same reason. Reconfigures when the file changes, so editing it and
# rebuilding is enough; VCAD_THEME=<file> overrides it at start without a rebuild.
set(_theme_src "${CMAKE_CURRENT_SOURCE_DIR}/themes/inventor.toml")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_theme_src}")
file(READ "${_theme_src}" _theme_text)
set(_theme_cpp "${CMAKE_CURRENT_BINARY_DIR}/theme_data.cpp")
file(WRITE "${_theme_cpp}.in"
  "// Generated from ${_theme_src}. Do not edit.\n"
  "extern \"C\" const char cad_libgui_theme[] = R\"vcadtheme(${_theme_text})vcadtheme\";\n")
configure_file("${_theme_cpp}.in" "${_theme_cpp}" COPYONLY)

# The ribbon's icons: SVG sources in icons/, one per RibbonItem::iconName, compiled in as text and
# rasterised once at start (src/Icons.cpp). The name is the file's stem.
file(GLOB _icons CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/icons/*.svg")
list(SORT _icons)
set(_icons_body "")
foreach(_icon IN LISTS _icons)
  get_filename_component(_name "${_icon}" NAME_WE)
  file(READ "${_icon}" _svg)
  string(APPEND _icons_body "    {\"${_name}\", R\"vcadsvg(${_svg})vcadsvg\"},\n")
endforeach()
set(_icons_cpp "${CMAKE_CURRENT_BINARY_DIR}/icon_data.cpp")
file(WRITE "${_icons_cpp}.in"
  "// Generated from shell_libgui/icons/*.svg. Do not edit.\n"
  "#include \"Icons.h\"\n"
  "namespace cad::shell_libgui {\n"
  "const IconSource kIconSources[] = {\n${_icons_body}};\n"
  "const std::size_t kIconSourceCount = sizeof kIconSources / sizeof kIconSources[0];\n"
  "}\n")
configure_file("${_icons_cpp}.in" "${_icons_cpp}" COPYONLY)
find_path(NANOSVG_INCLUDE_DIR nanosvg.h PATH_SUFFIXES nanosvg REQUIRED)

add_executable(vcad_libgui
  src/main.cpp
  src/Shell.cpp
  src/Icons.cpp
  "${_icons_cpp}"
  src/gui/Gui.cpp
  "${_font_cpp}"
  "${_theme_cpp}")
target_include_directories(vcad_libgui PRIVATE src)
target_include_directories(vcad_libgui SYSTEM PRIVATE "${NANOSVG_INCLUDE_DIR}")
target_link_libraries(vcad_libgui PRIVATE cad::app cad_render libgui::libgui SDL3::SDL3)
cad_set_warnings(vcad_libgui)
# The generated font table is one very long line of literals; warnings there are noise.
set_source_files_properties("${_font_cpp}" PROPERTIES COMPILE_OPTIONS "-w")
set_target_properties(vcad_libgui PROPERTIES MACOSX_BUNDLE FALSE OUTPUT_NAME vcad_libgui)

# Shaders beside the binary, driven by the shader FILES (see shell_qt/CMakeLists.txt for why not
# POST_BUILD). Both sets: the scene's and the interface's.
get_target_property(_scene_shaders cad_shaders CAD_SHADER_OUTPUTS)
get_target_property(_ui_shaders cad_ui_shaders CAD_SHADER_OUTPUTS)
set(_stamp "${CMAKE_CURRENT_BINARY_DIR}/shaders.stamp")
add_custom_command(
  OUTPUT "${_stamp}"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_BINARY_DIR}/shaders" "$<TARGET_FILE_DIR:vcad_libgui>/shaders"
  COMMAND ${CMAKE_COMMAND} -E touch "${_stamp}"
  DEPENDS ${_scene_shaders} ${_ui_shaders}
  COMMENT "Copying shader binaries beside vcad_libgui")
add_custom_target(vcad_libgui_shaders ALL DEPENDS "${_stamp}")
add_dependencies(vcad_libgui_shaders cad_shaders cad_ui_shaders)
add_dependencies(vcad_libgui vcad_libgui_shaders)
