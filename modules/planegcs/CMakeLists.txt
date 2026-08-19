# planegcs — FreeCAD's 2D geometric constraint solver, vendored. See VENDORED.md.
#
# SHARED, not static, and this is a licence decision rather than a technical preference.
#
# planegcs is LGPL-2.1-or-later. LGPL section 6 lets a work that is not itself LGPL link against
# the library, but if it links STATICALLY the distributor must additionally ship whatever a user
# needs to relink the application against a modified version of the library. Dynamic linking
# satisfies that obligation by construction, which is exactly the reasoning ADR 0001 already
# applied to Qt. So this one target opts out of cad_core_library()'s always-static rule.
#
# That rule exists because vcpkg ships OCCT as a static archive, and linking it PRIVATE into
# several shared libraries gives each its own copy of OCCT's global state. planegcs does not touch
# OCCT at all — it is Eigen and Boost.Graph — so being shared cannot reintroduce that problem.

set(PLANEGCS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Mod/Sketcher/App/planegcs")

# SHARED on desktop, STATIC on iOS.
#
# An iOS app may only load dylibs it ships inside its own bundle, and CMake's rpath for a build-tree
# dylib names a directory on the BUILD MACHINE. The device cannot see it, so dyld kills the process
# before main runs — the app appears on the home screen, launches, and closes instantly, which reads
# as a crash in the tests rather than a packaging mistake. devicectl even reports exit code 0.
#
# Static side-steps it entirely, and costs nothing here: the reason this is shared is a comment about
# OCCT's global state, and planegcs does not touch OCCT.
set(PLANEGCS_LINKAGE SHARED)
if(IOS)
  set(PLANEGCS_LINKAGE STATIC)
endif()

add_library(planegcs ${PLANEGCS_LINKAGE}
  "${PLANEGCS_DIR}/GCS.cpp"
  "${PLANEGCS_DIR}/Constraints.cpp"
  "${PLANEGCS_DIR}/Geo.cpp"
  "${PLANEGCS_DIR}/SubSystem.cpp"
  "${PLANEGCS_DIR}/qp_eq.cpp"
)
add_library(cad::planegcs ALIAS planegcs)

# Windows produces an import library for a DLL only if the DLL EXPORTS something, and these
# vendored sources carry no __declspec(dllexport) -- they were never written to be a DLL. So the
# build produced planegcs.dll with no planegcs.lib, and everything downstream failed with
# LNK1104: cannot open file 'planegcs.lib'.
#
# WINDOWS_EXPORT_ALL_SYMBOLS is CMake's escape hatch for exactly this: it generates a .def listing
# every symbol, at link time, without touching the vendored sources -- which matters, because
# VENDORED.md's whole claim is that they are unmodified.
#
# THE OBVIOUS FIX IS THE WRONG ONE. Making this target STATIC on Windows would also make the error
# go away, and would quietly change our licensing position: planegcs is LGPL-2.1-or-later, and the
# SHARED above is a licence decision (see the header of this file), not a technical preference.
# Static linking would oblige us to ship what a user needs to relink against a modified solver.
# Anyone tempted to simplify this line should read the top of the file first.
set_target_properties(planegcs PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)

# The vendored sources are UNMODIFIED, which is why the include paths look the way they do:
#   - the upstream directory itself, for its own headers
#   - `shim/`, which supplies the FreeCAD headers planegcs includes with angle brackets
#     (FCConfig.h, Base/Console.h, boost_graph_adjacency_list.hpp)
#   - `Mod/Sketcher/SketcherGlobal.h` is reached by upstream's own `../../SketcherGlobal.h`, so it
#     needs no include path — only the mirrored directory layout, which is why the tree is nested.
# SYSTEM on the public include dir: planegcs's headers produce sign-conversion warnings under our
# warning set, and they surface in every FILE THAT INCLUDES THEM, not in planegcs's own build.
# Without SYSTEM, core/sketch inherits a wall of warnings from code we do not maintain -- which is
# how real warnings in our own code get lost.
target_include_directories(planegcs SYSTEM
  PUBLIC  "${PLANEGCS_DIR}"
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/shim")

find_package(Eigen3 REQUIRED NO_MODULE)
find_package(Boost REQUIRED)
# Both PUBLIC, because both leak through planegcs's own headers: GCS.h includes <Eigen/...> and
# Geo.h includes <boost/math/constants/constants.hpp>. Anything that includes GCS.h therefore needs
# both on its include path. Marking Boost PRIVATE compiled the library fine and then failed the
# first consumer, which is the usual way an interface dependency gets mislabelled.
target_link_libraries(planegcs PUBLIC Eigen3::Eigen Boost::headers)

# The Base/Console shim routes the solver's diagnostics into our log instead of stderr. PRIVATE:
# it is used by the shim only, and nothing about planegcs's interface exposes it.
target_link_libraries(planegcs PRIVATE cad::log)

# C++20: GCS.cpp uses <numbers>.
target_compile_features(planegcs PUBLIC cxx_std_20)

# Third-party code is not held to our warning settings. Our own -Werror would fail the build on
# warnings we are not going to fix, because fixing them means diverging from upstream and making
# every future sync a merge. Warnings from headers we include are silenced at the target instead —
# which is what upstream's boost_graph_adjacency_list.hpp wrapper was doing for itself.
if(MSVC)
  target_compile_options(planegcs PRIVATE /W0)
else()
  target_compile_options(planegcs PRIVATE -w)
endif()

# Symbols must be visible: SketcherExport is defined empty by our shim, so nothing is annotated,
# and a hidden-by-default build of a shared library would export nothing at all.
set_target_properties(planegcs PROPERTIES
  CXX_VISIBILITY_PRESET default
  VISIBILITY_INLINES_HIDDEN OFF)

if(APPLE)
  # Absolute install_name, matching what abi/ does. Without it, anything loading this from a
  # different working directory — the Rust test harness, a plugin host — fails at load time with a
  # library-not-found that names no reason.
  set_target_properties(planegcs PROPERTIES
    INSTALL_NAME_DIR "${CMAKE_BINARY_DIR}/modules/planegcs")
endif()
