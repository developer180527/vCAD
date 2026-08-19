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
    UITapGestureRecognizer *_tap;
    /// One baseline PER RECOGNISER. Sharing one between the orbit and the pan is what made adding
    /// a second finger mid-drag throw the view across the screen.
    CGPoint _lastOrbit;
    CGPoint _lastTwoFinger;
    CGFloat _lastScale;

    NSString *_rendererName;
    NSString *_lastError;
    /// What last touched the screen. See `fingerRadiusPixels`.
    UITouchType _lastTouchType;
    /// The last tap's result, for the diagnostics panel.
    NSString *_lastTapReport;
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

    // A tap selects. It coexists with the orbit pan rather than competing: UIKit only recognises a
    // tap when the finger did not travel, so a drag that starts as a touch and becomes a rotation
    // never also selects something at the point it started from.
    _tap = [[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(handleTap:)];
    [self addGestureRecognizer:_tap];

    // Pinch and two-finger pan must run together: a real hand does both at once, and a recognizer
    // that wins exclusively makes the view feel like it is fighting back.
    _pinch.delegate = (id<UIGestureRecognizerDelegate>)self;
    _panGesture.delegate = (id<UIGestureRecognizerDelegate>)self;
    _orbit.delegate = (id<UIGestureRecognizerDelegate>)self;

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

- (void)shutdown {
    [_link invalidate];
    _link = nil;
    _attached = NO;
    // The blocks FIRST. They are the retain cycle — each one captures SwiftUI state that holds this
    // view — so until they are gone this object cannot be released no matter who lets go of it.
    self.onStatus = nil;
    self.onDocumentChanged = nil;
    self.onStarted = nil;
    // Deleting the Controller is what shuts bgfx down (see its destructor: backend first, then the
    // surface). Nulled rather than left dangling because every method here checks it.
    delete _controller;
    _controller = nullptr;
}

- (void)dealloc {
    [self shutdown];
    [_link invalidate];
    // The recognizers were alloc'd here (+1) and retained again by `addGestureRecognizer:`; without
    // this the view's own teardown leaves three orphans behind.
    [_orbit release];
    [_panGesture release];
    [_pinch release];
    [_tap release];
    [_rendererName release];
    [_lastError release];
    [_lastTapReport release];
    [super dealloc];
}

/// Records what is touching the screen, before any gesture recogniser decides what the touch means.
///
/// `UITouch.type` is the only honest source for this: there is no "pencil gesture recogniser", and
/// `allowedTouchTypes` would let us build two separate recognisers at the cost of them competing
/// with each other. One recogniser, and the aperture asks what kind of touch it was.
- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    [super touchesBegan:touches withEvent:event];
    _lastTouchType = touches.anyObject.type;
}

/// Pinch and two-finger pan run together; the one-finger orbit runs with neither.
///
/// A real hand pinches and drags at the same time, and a recogniser that wins exclusively makes the
/// view feel like it is fighting back — so those two must be simultaneous. The orbit is the opposite
/// case: it is a DIFFERENT gesture from the two-finger ones, not a component of them, and letting it
/// run alongside is what made a model rotate while being panned.
- (BOOL)gestureRecognizer:(UIGestureRecognizer *)a
    shouldRecognizeSimultaneouslyWithGestureRecognizer:(UIGestureRecognizer *)b {
    if (a == _orbit || b == _orbit) return NO;
    return YES;
}

// MARK: - Bring-up

- (BOOL)start {
    if (_attached) return YES;
    // A shut-down view stays shut down. Restarting one would bring up a second bgfx context in a
    // process that can only have one.
    if (_controller == nullptr) return NO;

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
        if (self.onStarted) self.onStarted(NO, _lastError);
        return NO;
    }

    auto r = _controller->attachRenderer(static_cast<std::uint32_t>(pixels.width),
                                         static_cast<std::uint32_t>(pixels.height),
                                         (void *)self, scale);
    if (!r) {
        _lastError = [[NSString stringWithUTF8String:r.error().message.c_str()] retain];
        if (self.onStarted) self.onStarted(NO, _lastError);
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
    if (self.onStarted) self.onStarted(YES, @"");
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

/// # Adding or lifting a finger mid-gesture
///
/// Putting a second finger down while orbiting made the view jump wildly. Two causes, both of them
/// about STATE that outlives the gesture that owns it:
///
/// 1. **One shared `_lastPan` for two recognisers.** Each differences the current translation
///    against it, and each reset it on its own `.began`. Interleaved callbacks therefore differenced
///    against the *other* gesture's baseline, producing a delta the size of the whole gesture so
///    far. Each recogniser now keeps its own.
///
/// 2. **The orbit never stops when the pan starts.** A `UIPanGestureRecognizer` with
///    `maximumNumberOfTouches = 1` does not end when a second finger lands — it keeps tracking the
///    first finger. So both handlers ran, the model rotated *and* panned, and worse: when the second
///    finger lifted, the orbit's translation had been accumulating unseen the whole time, so its
///    next delta contained every pixel travelled during the two-finger phase. Hence the jump on the
///    way OUT of the gesture as well as into it.
///
/// The fix for (2) is to keep consuming the orbit's translation while the pan owns the gesture,
/// applying none of it. Suppressing the callback instead would leave exactly the stale baseline that
/// causes the jump — the deltas have to be swallowed, not skipped.
- (BOOL)twoFingerGestureActive {
    return _panGesture.state == UIGestureRecognizerStateBegan
           || _panGesture.state == UIGestureRecognizerStateChanged;
}

- (void)handleOrbit:(UIPanGestureRecognizer *)g {
    if (!_attached || _controller == nullptr) return;
    const CGPoint p = [g translationInView:self];
    if (g.state == UIGestureRecognizerStateBegan) {
        _lastOrbit = CGPointZero;
        return;
    }
    if ([self twoFingerGestureActive]) {
        // Two fingers mean pan, not orbit. The translation is still consumed so that resuming with
        // one finger continues from where the hand actually is.
        _lastOrbit = p;
        return;
    }
    // Deltas, not absolute translation: CameraController::orbit takes a movement in pixels, and
    // handing it the cumulative translation each callback spins the model at increasing speed.
    _controller->camera().orbit(static_cast<float>(p.x - _lastOrbit.x),
                                static_cast<float>(p.y - _lastOrbit.y));
    _lastOrbit = p;
    _controller->cameraChanged();
}

- (void)handlePan:(UIPanGestureRecognizer *)g {
    if (!_attached || _controller == nullptr) return;
    const CGPoint p = [g translationInView:self];
    if (g.state == UIGestureRecognizerStateBegan) {
        _lastTwoFinger = CGPointZero;
        return;
    }
    cad::render::Viewport vp{static_cast<std::uint32_t>(_lastPixelSize.width),
                             static_cast<std::uint32_t>(_lastPixelSize.height), 1.0f};
    _controller->camera().pan(static_cast<float>(p.x - _lastTwoFinger.x),
                              static_cast<float>(p.y - _lastTwoFinger.y), vp);
    _lastTwoFinger = p;
    _controller->cameraChanged();
}

- (void)handlePinch:(UIPinchGestureRecognizer *)g {
    if (!_attached || _controller == nullptr) return;
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

- (void)handleTap:(UITapGestureRecognizer *)g {
    const CGPoint p = [g locationInView:self];
    [self tapAtX:p.x y:p.y];
}

// MARK: - Selection

/// How big the pointer is, in device pixels.
///
/// # A stylus is not a finger, and the difference is exactly this number
///
/// This is the whole of what "stylus-aware selection" means at the hit-test level, and it is worth
/// stating because the temptation is to build something more elaborate. A pencil has a tip about a
/// millimetre across and the user can SEE where it is pointing; a fingertip is 16–20 mm across (MIT
/// Touch Lab) and is hidden under the hand. So:
///
///   * **Pencil or any stylus** — 6 points. Small enough to pick the edge you are touching, large
///     enough that a one-pixel edge is still catchable.
///   * **Finger** — 22 points, the radius of Apple's 44 pt minimum touch target.
///
/// `UITouchTypeStylus` covers third-party styluses that report as one, not only Apple Pencil, which
/// is the agnostic half of the promise: nothing here asks whether it is an Apple device.
///
/// Points scaled to pixels, because the pick buffer is indexed in device pixels — passing points
/// would give every pointer half its real size on a Retina display, which reads as an inaccurate
/// picker rather than as a units mistake.
- (std::uint32_t)fingerRadiusPixels {
    const CGFloat scale = self.window.screen.scale > 0 ? self.window.screen.scale
                                                       : UIScreen.mainScreen.scale;
    const CGFloat points = _lastTouchType == UITouchTypeStylus ? 6.0 : 22.0;
    return static_cast<std::uint32_t>(points * scale);
}

- (NSString *)pointerName {
    return _lastTouchType == UITouchTypeStylus ? @"stylus" : @"finger";
}

- (void)tapAtX:(CGFloat)x y:(CGFloat)y {
    if (!_attached || _controller == nullptr) return;
    const CGFloat scale = self.window.screen.scale > 0 ? self.window.screen.scale
                                                       : UIScreen.mainScreen.scale;
    const std::uint32_t px = static_cast<std::uint32_t>(MAX(0.0, x * scale));
    const std::uint32_t py = static_cast<std::uint32_t>(MAX(0.0, y * scale));
    const std::uint32_t radius = [self fingerRadiusPixels];

    // Additive is YES, always: a tablet has no modifier key to hold, so tap-to-add and tap-again-
    // to-remove is the only selection model touch can express. `Controller::select` is already a
    // toggle, which is the whole of the behaviour. Onshape states the same rule outright — "tap to
    // select, tap again to deselect".
    const auto candidates = _controller->candidatesAt(px, py, radius);
    const auto result = _controller->tapAt(px, py, radius, /*additive=*/true);

    // Recorded on EVERY tap, hit or miss.
    //
    // "I tap and nothing happens" has several causes that look identical from the outside: the
    // gesture never fired, the aperture found nothing, or something was selected and the highlight
    // is not visible. This line distinguishes all three, and a miss is the case with no other
    // evidence at all — a tap on empty space is silent by design.
    [_lastTapReport release];
    _lastTapReport = [[NSString stringWithFormat:@"%@ r=%u at %u,%u -> %zu candidate(s)%@",
                                                 [self pointerName], radius, px, py,
                                                 candidates.size(),
                                                 result.hit ? @", hit" : @", miss"] retain];

    if (!result.message.empty() && self.onStatus) {
        self.onStatus([NSString stringWithUTF8String:result.message.c_str()]);
    } else if (self.onStatus) {
        self.onStatus(_lastTapReport);
    }
    if (result.changed) _dirty = YES;
}

- (NSArray<NSDictionary<NSString *, NSString *> *> *)candidatesAtX:(CGFloat)x y:(CGFloat)y {
    NSMutableArray *out = [NSMutableArray array];
    if (!_attached) return out;
    const CGFloat scale = self.window.screen.scale > 0 ? self.window.screen.scale
                                                       : UIScreen.mainScreen.scale;
    for (const auto &c : _controller->candidatesAt(static_cast<std::uint32_t>(MAX(0.0, x * scale)),
                                                   static_cast<std::uint32_t>(MAX(0.0, y * scale)),
                                                   [self fingerRadiusPixels])) {
        [out addObject:@{
            @"label" : [NSString stringWithUTF8String:c.label.c_str()],
            @"slot" : [NSString stringWithFormat:@"%u", c.slot],
        }];
    }
    return out;
}

- (NSArray<NSString *> *)selectionLabels {
    if (_controller == nullptr) return @[];
    NSMutableArray *out = [NSMutableArray array];
    for (const auto id : _controller->selection()) {
        if (const auto object = _controller->document().find(id)) {
            [out addObject:[NSString stringWithUTF8String:object->label().c_str()]];
        }
    }
    return out;
}

// MARK: - Commands

- (void)runCommand:(NSString *)commandId {
    if (_controller == nullptr) return;
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
    if (_controller == nullptr) return @[];
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
    if (_controller == nullptr) return @[];
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
    if (_controller == nullptr) return NO;
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
    if (_controller == nullptr) return NO;
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
    if (_controller == nullptr) return @{@"attached" : @"shut down"};
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
        @"lastTap" : _lastTapReport ?: @"none yet",
        @"pointer" : [self pointerName],
        @"selected" : [NSString stringWithFormat:@"%zu", _controller->selection().size()],
    };
}

- (NSString *)rendererName {
    return _rendererName;
}

- (NSString *)lastError {
    return _lastError;
}

@end
