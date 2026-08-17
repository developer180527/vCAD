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

#include "cad/document/Document.h"
#include "cad/recompute/Engine.h"
#include "cad/sketch/Sketch.h"

namespace cad::features {

/// A registry populated with every built-in type. Plugins add to it afterwards.
[[nodiscard]] recompute::FeatureRegistry builtins();

/// Locates a face-placed sketch on the body it is drawn on, filling in its resolved frame.
///
/// Shared because there are now two callers with the same requirement and no room for them to
/// disagree. `computeSketch` needs the frame to build the profile; the shell needs the SAME frame
/// to turn a click into a sketch coordinate while editing. Two copies of "resolve this name, then
/// measure that plane" would eventually differ by an axis convention, and the symptom would be a
/// sketch whose geometry lands somewhere other than where the user drew it — silently, because
/// each copy would be self-consistent.
///
/// A no-op for a sketch on a global plane: those need no resolution and already know where they
/// are. Returns false when the sketch names a face that cannot be found, which is `NamingLost` for
/// the feature and "you cannot edit this in place" for the shell — never a fall back to a global
/// plane, which would move the user's geometry without telling them.
[[nodiscard]] bool resolveSketchFrame(const document::Document&, document::ObjectId sketchId,
                                      sketch::Sketch&);

}  // namespace cad::features
