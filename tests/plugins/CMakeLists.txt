# Real plugins, built as real shared libraries, for the loader to load.
#
# These exist because every plugin test before the loader called the host vtable directly from
# inside the test process. That exercises the contract and not the loading: no manifest was read,
# nothing was dlopen'd, no descriptor was checked against a manifest, and `cad_plugin_main` was
# never the entry point. A separately compiled library is the only thing that tests those.
#
# MODULE rather than SHARED: these are dlopen'd, never linked against, and on Windows a MODULE
# produces no import library -- which is correct, because nothing should be able to link one.
function(cad_add_test_plugin name)
  cmake_parse_arguments(P "" "MANIFEST_DIR" "SOURCES" ${ARGN})
  add_library(${name} MODULE ${P_SOURCES})

  # The plugin links the ABI HEADER and nothing else. It does not link cad_abi: everything it can
  # reach arrives through the CadHost vtable it is handed, which is the property that lets a plugin
  # built years ago keep working. A plugin that linked host symbols would be bound to this build.
  target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/abi/include)

  # Undefined symbols are expected at link time -- the host provides nothing to link against.
  if(APPLE)
    target_link_options(${name} PRIVATE -undefined dynamic_lookup)
  endif()

  # CMake gives a MODULE library `.so` on macOS, where a shared library is `.dylib`. The loader
  # resolves a bare manifest name with the platform's shared-library suffix, so pin the suffix to
  # match rather than teach the loader a second convention -- the code that runs at every startup
  # should have one rule, not one rule and an exception.
  if(APPLE)
    set_target_properties(${name} PROPERTIES SUFFIX ".dylib")
  endif()

  set_target_properties(${name} PROPERTIES
    PREFIX ""                      # `vcad_demo_plugin.dylib`, matching the manifest's `library`
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${P_MANIFEST_DIR}"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${P_MANIFEST_DIR}")

  # The manifest goes beside the binary, which is the layout the loader expects and the layout an
  # installer would produce. See docs/design/PLUGIN_CONTRACT.md §2.
  add_custom_command(TARGET ${name} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/${P_MANIFEST_DIR}/plugin.manifest"
            "$<TARGET_FILE_DIR:${name}>/plugin.manifest"
    COMMENT "Staging ${P_MANIFEST_DIR}/plugin.manifest beside the plugin")
endfunction()

cad_add_test_plugin(vcad_demo_plugin
  MANIFEST_DIR demo
  SOURCES demo/plugin.cpp)
