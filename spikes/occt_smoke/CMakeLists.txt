add_executable(spike_occt_smoke main.cpp)
target_include_directories(spike_occt_smoke PRIVATE ${OpenCASCADE_INCLUDE_DIR})
target_link_libraries(spike_occt_smoke PRIVATE
  TKernel TKMath TKG3d TKGeomBase TKBRep TKTopAlgo TKPrim TKBO TKBool TKFillet
  TKShHealing TKMesh TKDESTEP TKXSBase)
add_test(NAME spike_occt_smoke COMMAND spike_occt_smoke)
