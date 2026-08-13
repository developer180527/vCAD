#pragma once

// SHIM — not upstream. See ../../VENDORED.md.
//
// FreeCAD builds the Sketcher module as a shared library and decorates its exported symbols with
// SketcherExport. We build planegcs as a library of our own, so there is nothing to decorate:
// visibility is handled by the CMake target, not per-declaration.
//
// This lives at the path upstream expects (`../../SketcherGlobal.h` from planegcs/) so that the
// vendored sources compile completely unmodified.
#define SketcherExport
