#pragma once

/// vCAD's icon vocabulary.
///
/// The machinery — device pixel ratio, pen width, the `QIcon` itself, and the handful of glyphs
/// every professional application shares — lives in `proshell/Icons.h`. Only the shapes that mean
/// something specifically in CAD are here, because "what a fillet looks like" is exactly the part
/// of an icon set that an architecture or civil application has no use for.
///
/// Call `proshell::icon(name, size)` to get an icon. This header exists only to register the
/// vocabulary once at startup.

#include "proshell/Icons.h"

namespace cadqt {

/// Registers vCAD's glyphs with the process-wide icon set. Call once, before building any UI.
void registerCadIcons();

/// `icon("extrude")` reads better than `proshell::icon("extrude")` at thirty-odd call sites, and
/// the name is not ambiguous inside this application. Re-exported rather than wrapped so there is
/// still exactly one implementation.
using proshell::icon;

}  // namespace cadqt
