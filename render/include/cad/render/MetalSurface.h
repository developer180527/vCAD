#pragma once

#include <cstdint>

/// macOS-only: a CAMetalLayer with no window, for headless rendering.
///
/// bgfx's Metal backend refuses to initialise without a layer even when nothing is presented to
/// the screen, so offscreen mode has to supply one. See render/src/MetalSurface.mm for the exact
/// bgfx source that requires it, and for why a layer — rather than an NSWindow or NSView — is the
/// only handle that can be passed safely from the thread calling bgfx::init.
namespace cad::render {

#if defined(__APPLE__)

/// Creates a standalone CAMetalLayer. Returns nullptr on failure. Caller owns it and must pass it
/// to destroyMetalLayer.
[[nodiscard]] void* createOffscreenMetalLayer(std::uint32_t width, std::uint32_t height);

/// Attaches a fresh CAMetalLayer to an NSView and returns it, for ON-SCREEN rendering.
///
/// `nsView` is what Qt's winId() yields for a widget with WA_NativeWindow. The layer is what gets
/// handed to bgfx — never the view itself, for the deadlock reason above.
///
/// Must be called on the main thread: it touches the view hierarchy.
[[nodiscard]] void* createMetalLayerForView(void* nsView, std::uint32_t width,
                                            std::uint32_t height, double scale);

/// Resizes a layer's drawable. Call on resize, before bgfx::reset, or the swap chain and the
/// layer disagree about how big the surface is. Null-safe.
void resizeMetalLayer(void* handle, std::uint32_t width, std::uint32_t height, double scale);

/// Releases a layer from either constructor. Null-safe.
void destroyMetalLayer(void* handle);

#endif

}  // namespace cad::render
