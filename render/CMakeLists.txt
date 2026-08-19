# render/ — tessellation, scene assembly, and the backend seam.
#
# May include bgfx. May NOT include Qt (tools/check_layering.py enforces it), and must never
# be a route for GPU types to reach core/ — that would end the iPad port.
find_package(bgfx CONFIG REQUIRED)

set(CAD_RENDER_SOURCES
  src/Tessellate.cpp
  src/MeshCache.cpp
  src/Scene.cpp
  src/Camera.cpp
  src/NullBackend.cpp
  src/BgfxBackend.cpp
)
# Objective-C++, and only on Apple platforms: bgfx's Metal backend cannot initialise without a
# CAMetalLayer, so headless rendering has to create one. See src/MetalSurface.mm.
# macOS only, NOT Apple generally. MetalSurface.mm is written against AppKit's NSView; iOS has no
# AppKit, and its equivalent is a UIView backed by a CAMetalLayer. Porting it is real work the iPad
# app will need — and work that belongs with the iPad app, not with this milestone: the acceptance
# suite drives the renderer through NullBackend and never asks for a surface at all.
#
# Excluded rather than #ifdef'd inside the file, so "iOS has no surface yet" is visible in the build
# description instead of being a silently empty translation unit.
if(APPLE AND NOT IOS)
  list(APPEND CAD_RENDER_SOURCES src/MetalSurface.mm)
elseif(IOS)
  list(APPEND CAD_RENDER_SOURCES src/MetalSurfaceIOS.cpp)
endif()

cad_core_library(cad_render ${CAD_RENDER_SOURCES})

if(APPLE)
  # QuartzCore and Metal exist on both; AppKit is macOS-only and is needed only by
  # MetalSurface.mm, which iOS does not build. Linking it there fails with
  # "framework 'AppKit' not found" — a confusing error for a file that is not in the target.
  target_link_libraries(cad_render PRIVATE "-framework QuartzCore" "-framework Metal")
  if(NOT IOS)
    target_link_libraries(cad_render PRIVATE "-framework AppKit")
  endif()
endif()

# Shaders land next to the build's binaries; BgfxBackend looks for them under CAD_SHADER_DIR,
# defaulting to ./shaders relative to the working directory.
if(CAD_SHADERS_AVAILABLE)
  cad_compile_shaders(cad_shaders
    SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders"
    OUTPUT_DIR "${CMAKE_BINARY_DIR}/shaders"
    SHADERS vs_shaded fs_shaded vs_edge fs_edge fs_pick)
  add_dependencies(cad_render cad_shaders)
endif()

target_include_directories(cad_render PUBLIC include)
target_link_libraries(cad_render PUBLIC cad::kernel cad::naming cad::document cad::recompute)
target_link_libraries(cad_render PRIVATE bgfx::bgfx bgfx::bx)
target_include_directories(cad_render PRIVATE ${OpenCASCADE_INCLUDE_DIR})
target_link_libraries(cad_render PRIVATE
  TKernel TKMath TKG2d TKG3d TKGeomBase TKBRep TKTopAlgo TKGeomAlgo TKMesh)
