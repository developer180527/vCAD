# render/ — tessellation, scene assembly, and the backend seam.
#
# May include bgfx. May NOT include Qt (tools/check_layering.py enforces it), and must never
# be a route for GPU types to reach core/ — that would end the iPad port.
cad_core_library(cad_render
  src/Tessellate.cpp
  src/MeshCache.cpp
  src/Scene.cpp
  src/Camera.cpp
  src/NullBackend.cpp
)

target_include_directories(cad_render PUBLIC include)
target_link_libraries(cad_render PUBLIC cad::kernel cad::naming cad::document cad::recompute)
target_include_directories(cad_render PRIVATE ${OpenCASCADE_INCLUDE_DIR})
target_link_libraries(cad_render PRIVATE
  TKernel TKMath TKG2d TKG3d TKGeomBase TKBRep TKTopAlgo TKGeomAlgo TKMesh)
