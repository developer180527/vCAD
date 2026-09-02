# Not a spike: spikes are throwaway, and this is an instrument we keep pointing at new corpora.
# Not a test either -- the files it reads cannot live in the repository, so it takes a path.
add_executable(vcad_import_probe main.cpp)
target_link_libraries(vcad_import_probe PRIVATE cad::io cad::naming cad::kernel assetlib)
cad_set_warnings(vcad_import_probe)
