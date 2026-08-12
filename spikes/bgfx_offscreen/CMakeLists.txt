add_executable(spike_bgfx_offscreen main.cpp)
# No windowing library. Offscreen rendering needs no window and MUST NOT have one on macOS:
# passing a window handle deadlocks Metal inside bgfx::init. See the platformData comment in
# render/src/BgfxBackend.cpp. This also makes the spike runnable with no window server at all —
# over ssh, in a container, or from an agent — which is the whole point of an offscreen check.
target_link_libraries(spike_bgfx_offscreen PRIVATE cad::core cad::render)
# Not registered with ctest: CI has no GPU. Run it by hand on a machine that does.
