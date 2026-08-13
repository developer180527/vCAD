// A CAMetalLayer with no window behind it.
//
// bgfx's Metal backend hard-requires a layer even when every view renders to an offscreen
// framebuffer — renderer_mtl.mm, RendererContextMtl::init:
//
//     if (NULL == m_mainFrameBuffer.m_swapChain->m_metalLayer) { release(m_device); return false; }
//
// so there is no windowless Metal path in bgfx, and asking for one yields a silent Noop fallback.
// A layer is cheap and does not need a window, though: `[[CAMetalLayer alloc] init]` gives a
// perfectly good surface that is simply never composited by anyone.
//
// Passing a CAMetalLayer is also the ONLY safe way to hand bgfx a surface from the main thread.
// Given an NSWindow or NSView, SwapChainMtl::init has to build the layer on the main thread, so
// it posts a block to the main run loop and waits — while the main thread is parked inside
// bgfx::init waiting for the render thread. That is a guaranteed deadlock, and it cost us a
// day. SwapChainMtl::init takes a CAMetalLayer directly, with no thread hop at all:
//
//     if ([nvh isKindOfClass:[CAMetalLayer class]]) { m_metalLayer = (CAMetalLayer*)_nwh; }
//
// The Qt viewport should therefore create its own layer on the NSView and pass THAT, rather than
// passing the view. Same function, different owner.
//
// Not compiled with ARC, matching the rest of the project: ownership is explicit below.

#include "cad/render/MetalSurface.h"

#if defined(__APPLE__)

#include <QuartzCore/CAMetalLayer.h>

namespace cad::render {

void* createOffscreenMetalLayer(std::uint32_t width, std::uint32_t height) {
    // alloc/init rather than +layer: +layer returns an autoreleased object, and this one has to
    // outlive the autorelease pool of whatever thread happened to call us.
    CAMetalLayer* layer = [[CAMetalLayer alloc] init];
    if (layer == nil) return nullptr;
    // bgfx assigns .device and .pixelFormat itself during init, and resets drawableSize on
    // reset(); setting the size here only matters for the frames before the first reset.
    layer.drawableSize = CGSizeMake(width, height);

    // THE reason the offscreen path was slow. A CAMetalLayer paces nextDrawable to the display's
    // refresh by default, so every bgfx::frame() blocked for up to a vsync interval — and a
    // readback has to pump at least two frames before the pixels are valid. That put a hard
    // ceiling of about 30ms on a capture and measured at 41.5 ms/frame, of which submit was
    // 0.0 ms: the entire cost was waiting for a display that never shows this layer to anyone.
    //
    // Nothing composites this surface. Every view renders into an offscreen framebuffer and we
    // read that back; the layer exists only because bgfx refuses to create a Metal device
    // without one. Pacing it to the screen buys nothing and costs everything.
    if ([layer respondsToSelector:@selector(setDisplaySyncEnabled:)]) {
        layer.displaySyncEnabled = NO;
    }
    return static_cast<void*>(layer);
}

void destroyMetalLayer(void* handle) {
    if (handle == nullptr) return;
    CAMetalLayer* layer = static_cast<CAMetalLayer*>(handle);
    [layer release];
}

}  // namespace cad::render

#endif  // __APPLE__
