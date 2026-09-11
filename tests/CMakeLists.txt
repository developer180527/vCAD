include(Catch)

# ── the suite is the DIRECTORY ───────────────────────────────────────────────────────────
#
# Every .cpp under acceptance/ is compiled into the suite, discovered rather than listed.
#
# # Why not a list
#
# A list of 71 filenames is a file every contributor and every concurrent session has to edit to
# add one test, and they all edit the same few lines of it. In this repo that is not theoretical:
# concurrent sessions collide here constantly, and the tree routinely carries this file modified
# with another session's source line in it while that source file is still untracked -- so anyone
# staging this file has to pick their own line out of someone else's first.
#
# No commit has actually shipped a line without its file; the history was checked rather than
# assumed. The cost so far is the collisions, not a broken build.
#
# The worse failure is quieter. A test file present in the directory but missing from the list
# compiles nowhere, runs never, and reports nothing: the suite is green and one file's worth of
# assertions is simply absent. Nothing in a list can detect its own omissions. A directory can.
#
# CONFIGURE_DEPENDS makes the build re-glob when the directory changes, so a new file is picked up
# by `cmake --build` without anyone remembering to re-run cmake. That costs a directory stat per
# build, which is the whole price.
file(GLOB CAD_ACCEPTANCE_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/acceptance/*.cpp")
list(SORT CAD_ACCEPTANCE_SOURCES)   # deterministic link order, so a build is reproducible

# Compiled only where the Rust library exists, so it is removed from the discovered set and added
# back below under its own condition.
list(REMOVE_ITEM CAD_ACCEPTANCE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/acceptance/rust_boundary.cpp")

# An empty glob would produce a test binary with no tests in it, which passes. That is the exact
# failure this whole arrangement exists to prevent, so it is an error rather than a warning: a
# renamed or moved directory has to break the build, not the reporting.
if(NOT CAD_ACCEPTANCE_SOURCES)
  message(FATAL_ERROR "tests: no sources found in acceptance/ -- has the directory moved?")
endif()
# main.cpp owns main() for the cross-process determinism test. Without it the suite still links,
# against Catch2's own main, and that test silently stops being able to re-execute the binary.
if(NOT "${CMAKE_CURRENT_SOURCE_DIR}/acceptance/main.cpp" IN_LIST CAD_ACCEPTANCE_SOURCES)
  message(FATAL_ERROR "tests: acceptance/main.cpp is missing -- it owns main() for the suite")
endif()

add_executable(cad_tests ${CAD_ACCEPTANCE_SOURCES})

target_include_directories(cad_tests PRIVATE acceptance
  ${CMAKE_SOURCE_DIR}/abi/include
  # OCCT's headers, so the About test can read the kernel version from the SAME macro the About box
  # does. Comparing against a literal here would only prove the literal matches itself.
  ${OpenCASCADE_INCLUDE_DIR})
# Catch2::Catch2, not Catch2WithMain: acceptance/main.cpp owns main() so the
# cross-process determinism test can re-execute this binary.
target_link_libraries(cad_tests PRIVATE cad::core Catch2::Catch2)

# The Rust boundary test only exists when the Rust library does. Guarded rather than assumed
# because CAD_REQUIRE_RUST is OFF while the library is a stub: a contributor without a Rust
# toolchain still gets a green suite, minus this one file.
if(TARGET cad::parse)
  target_sources(cad_tests PRIVATE acceptance/rust_boundary.cpp)
  target_link_libraries(cad_tests PRIVATE cad::parse)
endif()

# `dxf_differential.cpp` is in the discovered set unconditionally and SKIPS at runtime when the
# build has no Rust reader, rather than vanishing from the suite. A test that disappears when a
# dependency is absent reports the same green as one that ran.
#
# `plugin_manifest.cpp` reaches into abi/src by relative path rather than through a public header.
# Deliberate: the manifest parser is an internal detail of the loader, not part of the C ABI, and
# giving it a public header would invite a plugin to depend on the host's own parsing of its
# manifest.

# cad::app for face_pick: picking is driven through Controller, because "a click at these pixels
# means that face" is a model rule that must exist on both shells, not a Qt one.
target_link_libraries(cad_tests PRIVATE cad::abi cad::app)
target_link_libraries(cad_tests PRIVATE cad::sketch)
# catch_discover_tests runs the suite from the build tree, so a relative path to a fixture finds
# nothing. Baked in rather than copied beside the binary: the fixture is also the Rust fuzzer's
# seed corpus, and two copies of a corpus drift.
target_compile_definitions(cad_tests PRIVATE CAD_REPO_ROOT="${CMAKE_SOURCE_DIR}")
cad_set_warnings(cad_tests)

# The demo plugin is a loadable shared library, which iOS has no concept of for third-party code:
# an app may only dlopen what it shipped, signed with its own identity. The plugin TESTS still
# compile there and skip with a reason (tests/acceptance/Platform.h); it is only the fixture that
# cannot exist, so building it is skipped rather than the tests that use it.
if(NOT IOS)
  add_subdirectory(plugins)
  add_dependencies(cad_tests vcad_demo_plugin)
endif()
target_compile_definitions(cad_tests PRIVATE
  CAD_TEST_PLUGIN_DIR="${CMAKE_CURRENT_BINARY_DIR}/plugins")

# ── iOS: the suite has to be an app bundle ──────────────────────────────────────────────
#
# iOS cannot execute a bare binary. There is no shell, no working directory a process can be
# launched in, and no way to hand argv to something that is not a bundle — so verifying that the
# core "passes its tests on the iPad" means shipping the suite AS an app and reading its output back
# over the wire.
#
# The suite itself is unchanged: same sources, same Catch2 main, same assertions. Only the packaging
# differs, which is the point — a test that had to be rewritten to run on the device would no longer
# be evidence about the code that runs on the desktop.
if(IOS)
  # Named rather than sniffed at each use site, so a test can say WHY it is skipping.
  target_compile_definitions(cad_tests PRIVATE CAD_PLATFORM_IOS=1)
  set_target_properties(cad_tests PROPERTIES
    MACOSX_BUNDLE TRUE
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "dev.vcad.tests"
    # AND the plist variable, which is a different thing. CMake fills its bundle Info.plist template
    # from MACOSX_BUNDLE_GUI_IDENTIFIER; the Xcode attribute above only sets the build setting. With
    # just the attribute the app signs and validates and then devicectl refuses it: "not a valid
    # bundle ... ensure CFBundleIdentifier is set". Both are needed, and they must agree.
    MACOSX_BUNDLE_GUI_IDENTIFIER "dev.vcad.tests"
    MACOSX_BUNDLE_BUNDLE_NAME "cad_tests"
    MACOSX_BUNDLE_BUNDLE_VERSION "1"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0"
    # A device build must be signed, and the suite needs no entitlements beyond running.
    XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Automatic"
    # Catch2 writes to stdout, which devicectl's --console streams back. No file I/O, so no
    # sandbox questions: the fixtures that need a path use the app's own container.
    XCODE_ATTRIBUTE_ENABLE_BITCODE "NO")
endif()

# Discovery RUNS the binary to enumerate tests, which is impossible when the binary is for another
# architecture. On iOS the suite is driven by launching the bundle on the device instead.
if(NOT IOS)
  catch_discover_tests(cad_tests)
endif()

# M1 is done when this passes.
add_test(NAME m1_acceptance COMMAND cad_tests "[m1]")

if(TARGET cad::parse)
  # Registered as its own ctest entry rather than folded into an existing tag: when the Rust
  # integration breaks it should be named in the failure list, not hidden inside a suite about
  # geometry.
  add_test(NAME rust_boundary COMMAND cad_tests "[rust]")
endif()

# Named separately for the same reason: "the two DXF readers disagree" is a specific and alarming
# finding, and it should say so in the failure list.
add_test(NAME dxf_differential COMMAND cad_tests "[differential]")
