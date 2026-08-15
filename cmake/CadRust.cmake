# Building the Rust static library and handing it to CMake as an ordinary target.
#
# The awkward part of this is not compiling Rust; it is making a cargo-produced archive behave
# like something `target_link_libraries` understands, on three platforms, without the build ever
# reaching the network. Each of the decisions below is one of those problems.

function(cad_add_rust_library)
  find_program(CARGO_EXECUTABLE cargo)
  if(NOT CARGO_EXECUTABLE)
    if(CAD_REQUIRE_RUST)
      message(FATAL_ERROR
        "cargo was not found, and CAD_REQUIRE_RUST is ON. Install Rust (https://rustup.rs) or "
        "configure with -DCAD_REQUIRE_RUST=OFF to build without the Rust components.")
    endif()
    # A stub with no callers can be skipped; the day core/ actually depends on it, flip
    # CAD_REQUIRE_RUST to ON by default and this becomes a hard error instead. Deliberately a
    # WARNING rather than silence: a build that quietly omits a component is how someone spends
    # an afternoon on a missing symbol.
    message(WARNING "cargo not found; skipping the Rust library (CAD_REQUIRE_RUST=OFF)")
    return()
  endif()

  set(_crate_dir "${CMAKE_SOURCE_DIR}/rust/cad-parse")

  # Cargo's own profile names, which are not CMake's. Anything that is not a debug build gets the
  # release profile: a debug-profile Rust archive inside an optimised C++ binary is a parser
  # running an order of magnitude slower than the code around it, which reads as "Rust is slow"
  # rather than as a build misconfiguration.
  if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(_cargo_profile_flag "")
    set(_cargo_profile_dir "debug")
  else()
    set(_cargo_profile_flag "--release")
    set(_cargo_profile_dir "release")
  endif()

  # Out of the source tree, and per-CMake-build-dir. Two CMake build directories (this project
  # has `build` and `build-qt`) sharing one cargo target directory serialise on cargo's lock and
  # rebuild each other's artefacts.
  set(_target_dir "${CMAKE_BINARY_DIR}/rust-target")

  if(MSVC)
    set(_lib_name "cad_parse.lib")
  else()
    set(_lib_name "libcad_parse.a")
  endif()
  set(_lib_path "${_target_dir}/${_cargo_profile_dir}/${_lib_name}")

  # The link libraries Rust's std needs, ASKED FOR rather than hardcoded per platform.
  #
  # `rustc --print native-static-libs` is the supported way to discover this, and it is right by
  # construction across platforms and toolchain versions. A hand-maintained list per OS is the
  # obvious alternative and is wrong the first time std's requirements change under us — as a
  # missing-symbol error at link time in someone else's CI.
  execute_process(
    COMMAND ${CMAKE_COMMAND} -E echo "pub fn probe() {}"
    OUTPUT_FILE "${CMAKE_BINARY_DIR}/rust_probe.rs")
  execute_process(
    COMMAND rustc --print native-static-libs --crate-type staticlib
            -o "${CMAKE_BINARY_DIR}/rust_probe.a" "${CMAKE_BINARY_DIR}/rust_probe.rs"
    ERROR_VARIABLE _probe_output
    OUTPUT_QUIET
    RESULT_VARIABLE _probe_result)
  set(_native_libs "")
  if(_probe_result EQUAL 0 AND _probe_output MATCHES "native-static-libs: ([^\n]*)")
    string(STRIP "${CMAKE_MATCH_1}" _native_libs_raw)
    separate_arguments(_native_libs UNIX_COMMAND "${_native_libs_raw}")
    message(STATUS "Rust std needs: ${_native_libs_raw}")
  else()
    message(WARNING "could not query rustc for native-static-libs; link errors may follow")
  endif()

  # --offline: `cmake --build` must never reach the network. The crate has no dependencies, so
  #   this costs nothing today and turns "we do not fetch at build time" from a policy into a
  #   build failure the moment someone adds a dependency without vendoring it.
  # --locked: the lockfile is authoritative; cargo may not quietly update it mid-build.
  file(GLOB_RECURSE _crate_sources CONFIGURE_DEPENDS "${_crate_dir}/src/*.rs")
  if(NOT _crate_sources)
    message(FATAL_ERROR "cad_add_rust_library: no .rs sources under ${_crate_dir}/src")
  endif()

  add_custom_command(
    OUTPUT "${_lib_path}"
    COMMAND ${CARGO_EXECUTABLE} build ${_cargo_profile_flag} --offline --locked
            --target-dir "${_target_dir}"
    WORKING_DIRECTORY "${_crate_dir}"
    # Depended on by FILE, so editing the source rebuilds -- CMake does not watch directories for
    # content changes, so naming the crate directory would not work.
    #
    # GLOBBED, not listed. The first version named src/lib.rs explicitly, which was correct while
    # that was the only source and silently wrong the moment the crate grew a second file: edits to
    # the DXF parser did not rebuild the library, and the C++ side went on linking a stale archive.
    # That is a bad failure anywhere and a dangerous one here, because the test comparing the two
    # DXF readers then compares the NEW dime path against an OLD Rust one and reports the
    # disagreements of a version nobody is running.
    #
    # CONFIGURE_DEPENDS makes the glob re-run at build time, so a newly added source is picked up
    # without a manual reconfigure. It costs a directory scan per build, which against silently
    # linking stale code is not a close call.
    DEPENDS ${_crate_sources} "${_crate_dir}/Cargo.toml" "${_crate_dir}/Cargo.lock"
    COMMENT "Building cad-parse (Rust, ${_cargo_profile_dir}, offline)"
    VERBATIM)
  add_custom_target(cad_parse_build DEPENDS "${_lib_path}")

  add_library(cad_parse STATIC IMPORTED GLOBAL)
  set_target_properties(cad_parse PROPERTIES
    IMPORTED_LOCATION "${_lib_path}"
    INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}/rust/include")
  if(_native_libs)
    set_target_properties(cad_parse PROPERTIES INTERFACE_LINK_LIBRARIES "${_native_libs}")
  endif()
  # An IMPORTED target carries no build rule of its own, so anything linking it must be ordered
  # after the cargo command explicitly. Without this the first build races: the linker looks for
  # an archive cargo has not written yet, and it fails only on a clean tree — which is to say, in
  # CI and never locally.
  add_dependencies(cad_parse cad_parse_build)
  add_library(cad::parse ALIAS cad_parse)

  set(CAD_RUST_AVAILABLE ON PARENT_SCOPE)
endfunction()
