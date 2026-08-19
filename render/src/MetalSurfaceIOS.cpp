/// iOS has no AppKit, and therefore no surface yet.
///
/// `MetalSurface.mm` is written against `NSView`, which does not exist on iOS. The equivalent there
/// is a `UIView` whose backing layer is a `CAMetalLayer` — genuinely different code, and code that
/// belongs with the iPad application rather than with a milestone about whether the CORE ports.
///
/// These stubs exist so the renderer LINKS on iOS. `BgfxBackend` already treats a null surface as a
/// legible failure ("the renderer's surface could not be created") rather than a crash, so an iOS
/// build that tries to bring up the GPU gets that message instead of an undefined symbol at link
/// time or, worse, a silent no-op.
///
/// Nothing in the acceptance suite reaches here: it drives the scene through `NullBackend`, which
/// needs no surface at all. That is the point of `NullBackend` existing — the scene logic is
/// testable on a platform that cannot yet present a frame.
///
/// **When the iPad app is built, this file is what gets replaced**, not `MetalSurface.mm`. Keeping
/// the two apart means neither one accumulates `#ifdef`s about the other's windowing system.

#include "cad/render/MetalSurface.h"

namespace cad::render {

void* createOffscreenMetalLayer(std::uint32_t, std::uint32_t) { return nullptr; }

void* createMetalLayerForView(void*, std::uint32_t, std::uint32_t, double) { return nullptr; }

void resizeMetalLayer(void*, std::uint32_t, std::uint32_t, double) {}

void destroyMetalLayer(void*) {}

}  // namespace cad::render
