// The iOS surface: a CAMetalLayer taken from, or attached to, a UIView.
//
// The sibling of MetalSurface.mm, which is written against AppKit's NSView. Everything in that
// file's header comment about WHY bgfx is handed a layer rather than a view applies here verbatim
// — bgfx's SwapChainMtl::init accepts a CAMetalLayer directly and would otherwise hop to the main
// thread while the main thread is parked inside bgfx::init, deadlocking.
//
// # The one real difference from macOS
//
// AppKit lets a view adopt an arbitrary layer (`setLayer:` + `setWantsLayer:`). UIKit does not:
// a UIView's layer is created by UIKit and its CLASS is fixed by `+[UIView layerClass]`. So the
// two platforms arrive at the same place from opposite directions — on macOS we make a layer and
// give it to the view; on iOS the VIEW declares it wants a CAMetalLayer and we configure the one
// UIKit already made.
//
// That is why this takes the view's own layer when it is already a CAMetalLayer, and only falls
// back to adding a sublayer when it is not. The fallback exists so a caller that forgot to
// override `layerClass` still renders instead of silently drawing nothing; the sublayer path is
// worse, because its frame has to be maintained by hand on every bounds change.
//
// Not compiled with ARC, matching the rest of the project: ownership is explicit below, and the
// retain/release pairs here are the ones the Qt path already has.

#include "cad/render/MetalSurface.h"

#if defined(__APPLE__)

#include <QuartzCore/CAMetalLayer.h>
#include <UIKit/UIKit.h>

namespace cad::render {

void* createOffscreenMetalLayer(std::uint32_t width, std::uint32_t height) {
    CAMetalLayer* layer = [[CAMetalLayer alloc] init];
    if (layer == nil) return nullptr;
    layer.drawableSize = CGSizeMake(width, height);
    // No displaySyncEnabled on iOS — that property is macOS-only. Nothing composites this layer,
    // so there is nothing to pace against either.
    return static_cast<void*>(layer);
}

void* createMetalLayerForView(void* uiView, std::uint32_t width, std::uint32_t height,
                              double scale) {
    if (uiView == nullptr) return nullptr;
    // A plain cast, not a `__bridge` one: this file is compiled without ARC, where `__bridge` has
    // no meaning and only earns a warning.
    UIView* view = static_cast<UIView*>(uiView);

    CAMetalLayer* layer = nil;
    if ([view.layer isKindOfClass:[CAMetalLayer class]]) {
        // The normal path: the view declared `+layerClass` as CAMetalLayer, so UIKit made one and
        // keeps its frame in step with the view for us.
        layer = (CAMetalLayer*)view.layer;
    } else {
        // The fallback. Its frame is set once here and does NOT follow the view — a caller on this
        // path must resize it itself. Kept because a wrong-but-visible surface is diagnosable and
        // an invisible one is not.
        layer = [[CAMetalLayer alloc] init];
        if (layer == nil) return nullptr;
        layer.frame = view.bounds;
        [view.layer addSublayer:layer];
    }

    // contentsScale AND drawableSize both matter, and they mean different things. contentsScale is
    // how the layer maps to points when composited; drawableSize is the pixel dimensions of the
    // texture bgfx draws into. Set only the first and the surface renders at half resolution on a
    // Retina display; set only the second and it is scaled wrongly on screen.
    layer.contentsScale = scale;
    layer.drawableSize = CGSizeMake(width, height);
    layer.needsDisplayOnBoundsChange = YES;

    // Retained unconditionally so that destroyMetalLayer's release balances on both paths. The
    // view's own layer is owned by the view as well; the extra retain is what stops it going away
    // under bgfx if the view is torn down first.
    return static_cast<void*>([layer retain]);
}

void resizeMetalLayer(void* handle, std::uint32_t width, std::uint32_t height, double scale) {
    if (handle == nullptr) return;
    CAMetalLayer* layer = static_cast<CAMetalLayer*>(handle);
    layer.contentsScale = scale;
    layer.drawableSize = CGSizeMake(width, height);
}

void destroyMetalLayer(void* handle) {
    if (handle == nullptr) return;
    CAMetalLayer* layer = static_cast<CAMetalLayer*>(handle);
    [layer release];
}

}  // namespace cad::render

#endif  // __APPLE__
