# shell_ios/ — the SwiftUI iPad shell. The ONLY place Swift is allowed.
#
# Same rule as shell_qt: this directory is a view layer. Model rules live in app/ and core/, so a
# behaviour implemented here would have to be reimplemented in the eventual Android shell, and the
# two would disagree. See docs/design/IPAD_UX.md.
#
# The SwiftUI half talks to app/ through ONE Objective-C++ class, Bridge/CadViewport.mm. Swift can
# call C++ directly now, but not this C++: std::function observers, kernel::Result<T> and OCCT types
# in transitively included headers. An ObjC facade costs the same amount of code and imposes nothing
# on the shared layer, which matters because the desktop shell uses it too.

if(NOT IOS)
  message(STATUS "shell_ios: skipped — not an iOS build")
  return()
endif()

if(NOT CMAKE_GENERATOR STREQUAL "Xcode")
  # Swift needs the Xcode generator here anyway for signing, and the failure without it is a
  # compiler-detection error that says nothing about generators.
  message(FATAL_ERROR "shell_ios needs -G Xcode; got '${CMAKE_GENERATOR}'")
endif()

set(CAD_IOS_BUNDLE_ID "dev.vcad.app" CACHE STRING "Bundle identifier for the iPad app")

add_executable(vcad_ios MACOSX_BUNDLE
  Sources/vCADApp.swift
  Sources/HomeView.swift
  Sources/ProjectView.swift
  Sources/Viewport.swift
  Sources/Project.swift
  Sources/ProjectStore.swift
  Sources/Palette.swift
  Sources/Rail.swift
  Sources/Settings.swift
  Bridge/CadViewport.mm
)

target_link_libraries(vcad_ios PRIVATE cad::app cad::render)
target_include_directories(vcad_ios PRIVATE Bridge)

set_target_properties(vcad_ios PROPERTIES
  OUTPUT_NAME "vCAD"
  MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
  MACOSX_BUNDLE_BUNDLE_NAME "vCAD"
  MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
  MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
  # BOTH of these. The Xcode attribute is what the build settings use; MACOSX_BUNDLE_GUI_IDENTIFIER
  # is what CMake substitutes into the plist. Setting only one produces a bundle whose declared
  # identifier and signed identifier differ, which installs and then refuses to launch — the same
  # trap the test bundle hit.
  MACOSX_BUNDLE_GUI_IDENTIFIER "${CAD_IOS_BUNDLE_ID}"
  XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${CAD_IOS_BUNDLE_ID}"
  # iPad only. An iPhone build would need a different layout for every screen here, and claiming
  # support for a device nobody has laid out is how a "universal" app ships broken on half of it.
  XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "2"
  XCODE_ATTRIBUTE_SWIFT_VERSION "5.0"
  XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Automatic"
  # How Swift sees the bridge. One header, listing one class — see Bridge/vCAD-Bridging-Header.h
  # for why its staying one line is the measure of the boundary holding.
  XCODE_ATTRIBUTE_SWIFT_OBJC_BRIDGING_HEADER
    "${CMAKE_CURRENT_SOURCE_DIR}/Bridge/vCAD-Bridging-Header.h"
  # Manual retain/release, matching render/src/*.mm. The bridge holds one C++ object by raw pointer
  # and a handful of UIKit objects the view hierarchy already owns; ARC would buy nothing and would
  # make this file disagree with the Objective-C++ next to it in the renderer.
  XCODE_ATTRIBUTE_CLANG_ENABLE_OBJC_ARC "NO"
)

# The compiled shaders, inside the bundle.
#
# `BgfxBackend` looks for them beside the executable, which on iOS is inside the app bundle — a
# path the app knows and the renderer cannot guess, so the bridge registers it at startup with
# `setShaderDirectory`. Without this copy bgfx comes up, loads no program, and draws nothing at a
# perfectly good frame rate.
#
# Copied POST_BUILD rather than declared as bundle resources because the directory structure
# matters: the loader looks for <dir>/<profile>/<name>.bin, and a flat list of resources loses the
# profile level. Xcode signs the bundle after every build phase, so the copy lands inside the
# signature rather than invalidating it.
if(TARGET cad_shaders)
  add_dependencies(vcad_ios cad_shaders)
  add_custom_command(TARGET vcad_ios POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_BINARY_DIR}/shaders" "$<TARGET_BUNDLE_CONTENT_DIR:vcad_ios>/shaders"
    COMMENT "Copying shader binaries into vCAD.app")
else()
  message(FATAL_ERROR
    "shell_ios needs compiled shaders (cad_shaders). Configure with a host shaderc: "
    "-DBGFX_SHADERC=/path/to/shaderc, as tools/ios-app.sh does.")
endif()

message(STATUS "iPad shell: ${CAD_IOS_BUNDLE_ID}")
