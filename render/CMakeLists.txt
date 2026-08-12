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
if(APPLE)
  list(APPEND CAD_RENDER_SOURCES src/MetalSurface.mm)
endif()

cad_core_library(cad_render ${CAD_RENDER_SOURCES})

if(APPLE)
  target_link_libraries(cad_render PRIVATE "-framework QuartzCore" "-framework Metal")
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
