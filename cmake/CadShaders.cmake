# Compiles bgfx shaders with shaderc at build time.
#
# Compiled from source rather than checking in per-platform binary blobs. Blobs would mean
# three or four opaque files per shader that nobody can regenerate without the exact toolchain,
# and a shader edit that silently does nothing because someone forgot to rebuild one of them.
#
# The cost is a dependency on bgfx[tools], which pulls glslang and spirv-tools. Worth it.

# shaderc is a HOST tool: it runs on the machine doing the build, whatever it is building for.
# Searching the TARGET triplet works only by coincidence when target == host, and cross-compiling
# finds an arm64-ios shaderc that does not exist and could not be executed if it did. The iOS build
# failed exactly there, pointing at .../arm64-ios/tools/bgfx/shaderc.
#
# The host triplet is searched first, so a cross build picks the runnable tool and a native build is
# unaffected — VCPKG_HOST_TRIPLET equals VCPKG_TARGET_TRIPLET there.
# BGFX_SHADERC may be set explicitly (-DBGFX_SHADERC=...), which find_program then leaves alone.
# That is the escape hatch for a cross build whose host triplet did not install the tools.
find_program(BGFX_SHADERC
  NAMES shaderc
  PATHS "${VCPKG_INSTALLED_DIR}/${VCPKG_HOST_TRIPLET}/tools/bgfx"
        "${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_HOST_TRIPLET}/tools/bgfx"
        "${_VCPKG_INSTALLED_DIR}/${VCPKG_HOST_TRIPLET}/tools/bgfx"
        "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/tools/bgfx"
        "${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/tools/bgfx"
  NO_DEFAULT_PATH)

find_path(BGFX_SHADER_INCLUDE
  NAMES bgfx_shader.sh
  PATHS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/include/bgfx"
        "${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/include/bgfx"
  NO_DEFAULT_PATH)

if(NOT BGFX_SHADERC OR NOT BGFX_SHADER_INCLUDE)
  message(WARNING
    "shaderc or bgfx_shader.sh not found — GPU shaders will not be built. "
    "The renderer will initialise and report a missing-shader error rather than draw. "
    "Install bgfx with the [tools] feature.")
  set(CAD_SHADERS_AVAILABLE OFF CACHE INTERNAL "")
  return()
endif()
set(CAD_SHADERS_AVAILABLE ON CACHE INTERNAL "")

# Which shader profiles this platform needs. Deliberately only the native one plus the
# fallback: compiling every profile everywhere quadruples build time for binaries the machine
# can never load.
if(APPLE)
  set(_cad_shader_platform osx)
  set(_cad_shader_profiles metal)
elseif(WIN32)
  set(_cad_shader_platform windows)
  set(_cad_shader_profiles dx11)
else()
  set(_cad_shader_platform linux)
  set(_cad_shader_profiles spirv)
endif()

# shaderc needs the shader model spelled out per profile, and the flag differs by platform.
function(_cad_shader_profile_args profile type out_var)
  if(profile STREQUAL "metal")
    set(${out_var} --profile metal PARENT_SCOPE)
  elseif(profile STREQUAL "spirv")
    set(${out_var} --profile spirv PARENT_SCOPE)
  elseif(profile STREQUAL "dx11")
    if(type STREQUAL "vertex")
      set(${out_var} --profile s_5_0 -O 3 PARENT_SCOPE)
    else()
      set(${out_var} --profile s_5_0 -O 3 PARENT_SCOPE)
    endif()
  else()
    set(${out_var} "" PARENT_SCOPE)
  endif()
endfunction()

# cad_compile_shaders(<target> SOURCE_DIR <dir> OUTPUT_DIR <dir> SHADERS <name>...)
#
# Each name is a stem: vs_foo compiles as a vertex shader, fs_foo as a fragment shader, by
# prefix. Outputs land in <OUTPUT_DIR>/<profile>/<name>.bin, which is the layout
# BgfxBackend::shaderPath expects.
function(cad_compile_shaders target)
  cmake_parse_arguments(ARG "" "SOURCE_DIR;OUTPUT_DIR" "SHADERS" ${ARGN})

  set(_all_outputs)
  foreach(profile ${_cad_shader_profiles})
    file(MAKE_DIRECTORY "${ARG_OUTPUT_DIR}/${profile}")
    foreach(name ${ARG_SHADERS})
      if(name MATCHES "^vs_")
        set(type vertex)
      elseif(name MATCHES "^fs_")
        set(type fragment)
      elseif(name MATCHES "^cs_")
        set(type compute)
      else()
        message(FATAL_ERROR "shader '${name}' must start with vs_, fs_ or cs_")
      endif()

      _cad_shader_profile_args(${profile} ${type} _profile_args)
      set(_in "${ARG_SOURCE_DIR}/${name}.sc")
      set(_out "${ARG_OUTPUT_DIR}/${profile}/${name}.bin")

      add_custom_command(
        OUTPUT "${_out}"
        COMMAND "${BGFX_SHADERC}"
                -f "${_in}" -o "${_out}"
                --type ${type}
                --platform ${_cad_shader_platform}
                ${_profile_args}
                --varyingdef "${ARG_SOURCE_DIR}/varying.def.sc"
                -i "${BGFX_SHADER_INCLUDE}"
                -i "${ARG_SOURCE_DIR}"
        DEPENDS "${_in}" "${ARG_SOURCE_DIR}/varying.def.sc" "${ARG_SOURCE_DIR}/common.sh"
        COMMENT "shaderc ${profile}/${name}"
        VERBATIM)
      list(APPEND _all_outputs "${_out}")
    endforeach()
  endforeach()

  add_custom_target(${target} ALL DEPENDS ${_all_outputs})
  # Published so a consumer can depend on the FILES, not just on the target. A POST_BUILD copy on an
  # executable only runs when that executable relinks, so editing a shader alone left the old binary
  # sitting beside the app -- the edit compiled, was never copied, and the app ran the previous
  # shader with nothing to indicate why the change had no effect.
  set_property(TARGET ${target} PROPERTY CAD_SHADER_OUTPUTS "${_all_outputs}")
endfunction()
