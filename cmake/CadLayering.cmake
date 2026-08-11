# Layering enforcement.
#
# The single most important structural rule in this codebase:
#
#   core  -> OCCT, planegcs, assetlib, Eigen. Nothing else.
#   render-> core
#   app   -> core, render
#   shell_*-> app
#
# If core ever includes a Qt or bgfx header, the iPad port is dead and the plugin ABI is
# unshippable. This is cheap to enforce now and impossible to retrofit.

function(cad_add_layering_check)
  if(NOT CAD_STRICT_LAYERING)
    return()
  endif()
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(NOT Python3_Interpreter_FOUND)
    message(WARNING "Python3 not found; layering check disabled")
    return()
  endif()
  add_custom_target(cad_layering_check ALL
    COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_SOURCE_DIR}/tools/check_layering.py"
            "${CMAKE_SOURCE_DIR}"
    COMMENT "Checking layer include direction"
    VERBATIM)
endfunction()

# Marks a target as belonging to the core layer: no UI, no GPU, no platform toolkits.
function(cad_core_library name)
  # STATIC, always — never controlled by BUILD_SHARED_LIBS.
  #
  # vcpkg ships OCCT as static archives. Linking those PRIVATE into several SHARED libraries
  # gives EACH one its own copy of OCCT's global state — type registries, session singletons,
  # the plugin tables. A shape built against one copy handed to an algorithm running against
  # another fails in ways that look like geometry bugs: BRepFilletAPI_MakeFillet simply
  # reports IsDone() == false, with no hint that the cause is a duplicated static.
  #
  # We hit exactly that when core/io was added. Diagnosis cost real time because every
  # symptom pointed at the fillet.
  #
  # The rule: internal modules are static, and OCCT is linked exactly once per SHARED
  # boundary we deliberately create (cad_abi, the Python module). BUILD_SHARED_LIBS remains
  # ON for THIRD-PARTY LGPL dependencies — Qt and planegcs — where dynamic linking is a
  # licensing requirement (ADR 0001). That obligation says nothing about our own code.
  add_library(${name} STATIC ${ARGN})
  # cad_kernel -> cad::kernel (not cad::cad_kernel)
  string(REGEX REPLACE "^cad_" "" _short "${name}")
  add_library(cad::${_short} ALIAS ${name})
  cad_set_warnings(${name})
  set_target_properties(${name} PROPERTIES CAD_LAYER core)
endfunction()
