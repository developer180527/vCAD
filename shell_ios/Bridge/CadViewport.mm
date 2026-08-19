#import "CadViewport.h"

#include "cad/app/Controller.h"
#include "cad/render/BgfxBackend.h"

#import <QuartzCore/CAMetalLayer.h>

#include <string>

/// The shared layer, driven from UIKit.
///
/// # The three things that make a viewport appear at all
///
/// Each of these has silently produced a blank screen before, on the desktop, so they are called
/// out rather than left as lines of setup:
///
/// 1. **`+layerClass` is `CAMetalLayer`.** UIKit fixes a view's layer class at this class method
///    and nowhere else. Without it the view gets a plain CALayer, `createMetalLayerForView` takes
///    its sublayer fallback, and the surface is a layer nobody resizes.
/// 2. **The shader directory is registered.** `BgfxBackend` looks for compiled shaders beside the
///    executable by default, which on iOS is inside the bundle — a path this app knows and the
///    renderer cannot guess. Unset, bgfx loads no program and draws nothing at full speed.
/// 3. **The drawable is sized in PIXELS.** `bounds` is in points; a Retina iPad is 2× that. Passing
///    points renders a quarter of the surface and stretches it.
@implementation CadViewportView {
    cad::app::Controller *_controller;
    CADisplayLink *_link;
    BOOL _attached;
    /// Set by the observers below; the display link presents only when something changed. A CAD
    /// viewport is static most of the time, and redrawing a still model 120 times a second is how
    /// a tablet gets hot holding a picture.
    BOOL _dirty;
    CGSize _lastPixelSize;

    UIPanGestureRecognizer *_orbit;
    UIPanGestureRecognizer *_panGesture;
    UIPinchGestureRecognizer *_pinch;
    CGPoint _lastPan;
    CGFloat _lastScale;

    NSString *_rendererName;
    NSString *_lastError;
}

+ (Class)layerClass {
    return [CAMetalLayer class];
}

- (instancetype)initWithFrame:(CGRect)frame {
    self = [super initWithFrame:frame];
    if (!self) return nil;

    _controller = new cad::app::Controller();
    _rendererName = @"none";
    _lastError = @"";
    _dirty = YES;
    self.opaque = YES;
    self.backgroundColor = [UIColor colorWithRed:0xFA / 255.0
                                           green:0xFA / 255.0
                                            blue:0xF9 / 255.0
                                           alpha:1.0];
    self.multipleTouchEnabled = YES;

    // ONE finger orbits, TWO pan, pinch zooms.
    //
    // This is the tablet convention — Shapr3D, Fusion for iPad and Onshape all agree — and it is
    // the opposite of the desktop, where the left button selects and orbit needs a modifier. The
    // difference is not arbitrary: on a tablet there is no hover, so a drag on empty space cannot
    // mean "start a rubber-band selection" without stealing the only gesture a hand does naturally.
    _orbit = [[UIPanGestureRecognizer alloc] initWithTarget:self action:@selector(handleOrbit:)];
    _orbit.maximumNumberOfTouches = 1;
    [self addGestureRecognizer:_orbit];

    _panGesture = [[UIPanGestureRecognizer alloc] initWithTarget:self action:@selector(handlePan:)];
    _panGesture.minimumNumberOfTouches = 2;
    _panGesture.maximumNumberOfTouches = 2;
    [self addGestureRecognizer:_panGesture];

    _pinch = [[UIPinchGestureRecognizer alloc] initWithTarget:self action:@selector(handlePinch:)];
    [self addGestureRecognizer:_pinch];

    // Pinch and two-finger pan must run together: a real hand does both at once, and a recognizer
    // that wins exclusively makes the view feel like it is fighting back.
    _pinch.delegate = (id<UIGestureRecognizerDelegate>)self;
    _panGesture.delegate = (id<UIGestureRecognizerDelegate>)self;

    // Captured unretained, and safe without a weak reference — which this file could not use
    // anyway, being compiled without ARC.
    //
    // The lifetimes are nested rather than merely related: the Controller is owned by this view and
    // deleted in `dealloc`, so a callback cannot outlive the object it points at. A retained capture
    // would be the actual bug, making the view own a callback that owns the view.
    __unsafe_unretained CadViewportView *me = self;
    _controller->onViewChanged([me] { me->_dirty = YES; });
    _controller->onDocumentChanged([me] {
        me->_dirty = YES;
        if (me.onDocumentChanged) me.onDocumentChanged();
    });
    _controller->onStatus([me](const std::string &text) {
        if (!me.onStatus) return;
        me.onStatus([NSString stringWithUTF8String:text.c_str()]);
    });

    return self;
}

- (void)dealloc {
    [_link invalidate];
    // The recognizers were alloc'd here (+1) and retained again by `addGestureRecognizer:`; without
    // this the view's own teardown leaves three orphans behind.
    [_orbit release];
    [_panGesture release];
    [_pinch release];
    [_rendererName release];
    [_lastError release];
    delete _controller;
    [super dealloc];
}

- (BOOL)gestureRecognizer:(UIGestureRecognizer *)a
    shouldRecognizeSimultaneouslyWithGestureRecognizer:(UIGestureRecognizer *)b {
    return YES;
}

// MARK: - Bring-up

- (BOOL)start {
    if (_attached) return YES;

    const CGFloat scale = self.window.screen.scale > 0 ? self.window.screen.scale
                                                       : UIScreen.mainScreen.scale;
    const CGSize pixels = [self pixelSize];
    if (pixels.width < 1 || pixels.height < 1) return NO;   // laid out later; try again then

    // Point the renderer at the shaders inside the bundle. See note 2 in the class comment.
    NSString *shaders = [[NSBundle mainBundle] pathForResource:@"shaders" ofType:nil];
    if (shaders) {
        cad::render::setShaderDirectory(std::string(shaders.UTF8String));
    } else {
        _lastError = [@"The renderer's shaders are missing from the app bundle." retain];
        return NO;
    }

    auto r = _controller->attachRenderer(static_cast<std::uint32_t>(pixels.width),
                                         static_cast<std::uint32_t>(pixels.height),
                                         (void *)self, scale);
    if (!r) {
        _lastError = [[NSString stringWithUTF8String:r.error().message.c_str()] retain];
        return NO;
    }
    _attached = YES;
    _lastPixelSize = pixels;
    _rendererName = [[NSString stringWithUTF8String:_controller->rendererName().c_str()] retain];
    _lastError = @"";

    // Clear to Paper White's canvas, so the GPU surface and the SwiftUI chrome around it are the
    // same paper. A viewport that clears to black frames itself against the app.
    _controller->setViewportBackground(0xFA, 0xFA, 0xF9);
    _controller->refresh();
    _controller->fitView();

    _link = [CADisplayLink displayLinkWithTarget:self selector:@selector(tick:)];
    [_link addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
    _dirty = YES;
    return YES;
}

- (CGSize)pixelSize {
    const CGFloat scale = self.window.screen.scale > 0 ? self.window.screen.scale
                                                       : UIScreen.mainScreen.scale;
    return CGSizeMake(MAX(1.0, self.bounds.size.width * scale),
                      MAX(1.0, self.bounds.size.height * scale));
}

- (void)layoutSubviews {
    [super layoutSubviews];
    if (!_attached) {
        // The first layout is where a view finally has a size. Starting here rather than at init
        // is what makes rotation, Slide Over and a late window attachment all work without a
        // special case each.
        [self start];
        return;
    }
    const CGSize pixels = [self pixelSize];
    if (fabs(pixels.width - _lastPixelSize.width) < 1 &&
        fabs(pixels.height - _lastPixelSize.height) < 1) {
        return;
    }
    _lastPixelSize = pixels;
    const CGFloat scale = self.window.screen.scale > 0 ? self.window.screen.scale
                                                       : UIScreen.mainScreen.scale;
    ((CAMetalLayer *)self.layer).drawableSize = pixels;
    ((CAMetalLayer *)self.layer).contentsScale = scale;
    _controller->setViewportSize(static_cast<std::uint32_t>(pixels.width),
                                 static_cast<std::uint32_t>(pixels.height));
    _dirty = YES;
}

- (void)tick:(CADisplayLink *)link {
    if (!_attached || !_dirty) return;
    _dirty = NO;
    _controller->presentFrame();
}

// MARK: - Camera gestures

- (void)handleOrbit:(UIPanGestureRecognizer *)g {
    if (!_attached) return;
    const CGPoint p = [g translationInView:self];
    if (g.state == UIGestureRecognizerStateBegan) _lastPan = CGPointZero;
    // Deltas, not absolute translation: CameraController::orbit takes a movement in pixels, and
    // handing it the cumulative translation each callback spins the model at increasing speed.
    _controller->camera().orbit(static_cast<float>(p.x - _lastPan.x),
                                static_cast<float>(p.y - _lastPan.y));
    _lastPan = p;
    _controller->cameraChanged();
}

- (void)handlePan:(UIPanGestureRecognizer *)g {
    if (!_attached) return;
    const CGPoint p = [g translationInView:self];
    if (g.state == UIGestureRecognizerStateBegan) _lastPan = CGPointZero;
    cad::render::Viewport vp{static_cast<std::uint32_t>(_lastPixelSize.width),
                             static_cast<std::uint32_t>(_lastPixelSize.height), 1.0f};
    _controller->camera().pan(static_cast<float>(p.x - _lastPan.x),
                              static_cast<float>(p.y - _lastPan.y), vp);
    _lastPan = p;
    _controller->cameraChanged();
}

- (void)handlePinch:(UIPinchGestureRecognizer *)g {
    if (!_attached) return;
    if (g.state == UIGestureRecognizerStateBegan) {
        _lastScale = 1.0;
        return;
    }
    // Zoom takes wheel-like ticks. A pinch reports a cumulative scale factor, so the tick is the
    // RATIO since the last callback — which also makes the gesture symmetric: pinching out and
    // back in returns to where it started.
    const CGFloat ratio = g.scale / (_lastScale > 0 ? _lastScale : 1.0);
    _lastScale = g.scale;
    _controller->camera().zoom(static_cast<float>((ratio - 1.0) * 8.0));
    _controller->cameraChanged();
}

// MARK: - Commands

- (void)runCommand:(NSString *)commandId {
    const std::string id(commandId.UTF8String);
    for (const auto &c : _controller->commands()) {
        if (c.id != id) continue;
        if (c.enabled && !c.enabled(_controller->context())) return;

        const bool wasEmpty = _controller->stats().objects == 0;
        if (c.invoke) c.invoke();
        _controller->refresh();

        // Frame the model when the FIRST body appears.
        //
        // `fitView` on an empty document has nothing to frame, so the camera keeps whatever
        // distance it started with — and the first box is then drawn somewhere off screen, which
        // is indistinguishable from not being drawn at all. Only on the first body: re-framing
        // after every command would yank the view out from under someone mid-edit.
        if (wasEmpty && _controller->stats().objects > 0) _controller->fitView();

        _dirty = YES;
        return;
    }
}

- (NSArray<NSDictionary<NSString *, NSString *> *> *)commands {
    NSMutableArray *out = [NSMutableArray array];
    const auto ctx = _controller->context();
    for (const auto &c : _controller->commands()) {
        [out addObject:@{
            @"id" : [NSString stringWithUTF8String:c.id.c_str()],
            @"label" : [NSString stringWithUTF8String:c.label.c_str()],
            @"tooltip" : [NSString stringWithUTF8String:c.tooltip.c_str()],
            @"icon" : [NSString stringWithUTF8String:c.iconName.c_str()],
            @"enabled" : (!c.enabled || c.enabled(ctx)) ? @"1" : @"0",
        }];
    }
    return out;
}

- (NSArray<NSDictionary<NSString *, NSString *> *> *)tree {
    NSMutableArray *out = [NSMutableArray array];
    for (const auto &item : _controller->tree()) {
        [out addObject:@{
            @"label" : [NSString stringWithUTF8String:item.label.c_str()],
            @"kind" : [NSString stringWithUTF8String:item.type.c_str()],
            @"error" : [NSString stringWithUTF8String:item.error.c_str()],
        }];
    }
    return out;
}

- (void)fitCamera {
    if (!_attached) return;
    _controller->fitView();
    _dirty = YES;
}

- (BOOL)openDocumentAtPath:(NSString *)path {
    auto r = _controller->loadFrom(std::string(path.UTF8String));
    if (!r) {
        _lastError = [[NSString stringWithUTF8String:r.error().message.c_str()] retain];
        return NO;
    }
    _controller->refresh();
    _controller->fitView();
    _dirty = YES;
    return YES;
}

- (BOOL)saveDocumentAtPath:(NSString *)path {
    auto r = _controller->saveTo(std::string(path.UTF8String));
    if (!r) {
        _lastError = [[NSString stringWithUTF8String:r.error().message.c_str()] retain];
        return NO;
    }
    return YES;
}

- (BOOL)attached {
    return _attached;
}

- (NSDictionary<NSString *, NSString *> *)diagnostics {
    const auto s = _controller->stats();
    return @{
        @"renderer" : _rendererName ?: @"none",
        @"attached" : _attached ? @"yes" : @"no",
        @"presenting" : _controller->presentsDirectly() ? @"direct" : @"blit",
        @"objects" : [NSString stringWithFormat:@"%zu", s.objects],
        @"meshes" : [NSString stringWithFormat:@"%zu", s.uniqueMeshes],
        @"instances" : [NSString stringWithFormat:@"%zu", s.instances],
        @"triangles" : [NSString stringWithFormat:@"%zu", s.triangles],
        @"failed" : [NSString stringWithFormat:@"%zu", s.failed],
        @"error" : _lastError ?: @"",
    };
}

- (NSString *)rendererName {
    return _rendererName;
}

- (NSString *)lastError {
    return _lastError;
}

@end
