add_executable(spike_planegcs main.cpp)
target_link_libraries(spike_planegcs PRIVATE cad::planegcs)
# Not a ctest: this is the de-risking spike for a vendored dependency. It has no GPU or platform
# requirement, so it could become one -- but the real tests belong behind a sketch API in Rust.
