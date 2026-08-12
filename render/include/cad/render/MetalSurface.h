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

/// Releases a layer from createOffscreenMetalLayer. Null-safe.
void destroyMetalLayer(void* handle);

#endif

}  // namespace cad::render
