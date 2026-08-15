#pragma once

/// vCAD's built-in feature types: Box, Cylinder, Fillet, Chamfer, the booleans, Sketch, Extrude,
/// Translate and Import.
///
/// # Why this is not in core/recompute
///
/// It was, and it made the recompute ENGINE depend on the sketch solver — so a dependency graph
/// with dirty propagation, caching and rollback, which knows nothing about geometry, dragged in
/// planegcs and OCCT because the built-in catalogue happened to live in the same module.
///
/// The engine is the frame; these are the vocabulary. Exactly the split the desktop shell needed
/// between `proshell::ShellWindow` and vCAD's 294-line ribbon catalogue, and for the same reason:
/// a second application in another domain wants the frame and none of the vocabulary.
///
/// `FeatureRegistry::builtins()` used to be a static member of the registry, which put the same
/// coupling in the engine's own header — the registry declaring a factory for solids. It is a free
/// function here instead, so the engine's header names no feature type at all.

#include "cad/recompute/Engine.h"

namespace cad::features {

/// A registry populated with every built-in type. Plugins add to it afterwards.
[[nodiscard]] recompute::FeatureRegistry builtins();

}  // namespace cad::features
