#pragma once
// SHIM — not upstream. See ../VENDORED.md.
//
// Upstream wraps this include to silence warnings from Boost.Graph under FreeCAD's warning flags.
// We suppress them on the target instead (see CMakeLists.txt), so this just forwards.
#include <boost/graph/adjacency_list.hpp>
