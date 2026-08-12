add_executable(spike_scale main.cpp)
# No windowing library: offscreen needs no window. See spikes/bgfx_offscreen/CMakeLists.txt.
target_link_libraries(spike_scale PRIVATE cad::core cad::render)
# Not registered with ctest: needs a GPU, and it is a measurement rather than a pass/fail unit
# test. It does return non-zero when an architectural claim breaks, so it can gate a release.
