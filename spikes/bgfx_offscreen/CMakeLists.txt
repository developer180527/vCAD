add_executable(spike_bgfx_offscreen main.cpp)
find_package(glfw3 CONFIG REQUIRED)
target_link_libraries(spike_bgfx_offscreen PRIVATE cad::core cad::render glfw)
# Not registered with ctest: CI has no GPU. Run it by hand on a machine that does.
