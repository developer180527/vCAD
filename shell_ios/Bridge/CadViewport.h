#import <UIKit/UIKit.h>

/// The bridge between SwiftUI and the C++ application layer.
///
/// # Why a UIView and not a set of free functions
///
/// Everything the viewport needs — the document, the camera, the scene, the GPU surface and the
/// frame loop — has the same lifetime as the view on screen, and tying them to it means there is
/// no separate object to create, own, and forget to tear down. The view IS the session.
///
/// # Why Objective-C and not C++ interop
///
/// Swift can call C++ directly now, but not the parts of it this needs: `std::function` observers,
/// `kernel::Result<T>`, OCCT types in transitively included headers. An Objective-C facade over
/// Objective-C++ is the same amount of code and imposes no constraints on what the C++ side may
/// look like — which matters because `app/` is shared with the desktop shell and must not be bent
/// to suit this one.
///
/// **This header is Objective-C only.** No C++ appears in it, so it can be a bridging header. The
/// implementation is `.mm` and holds the C++ by pointer.
///
/// # What is deliberately NOT here
///
/// Anything that decides what a gesture MEANS beyond the camera. Selection filters, tool state,
/// what a tap resolves to — those are `app/`'s and must not be reinvented in a shell, or the iPad
/// and the desktop will disagree about the same model. See docs/design/IPAD_UX.md.
NS_ASSUME_NONNULL_BEGIN

@interface CadViewportView : UIView

/// Brings up bgfx against this view's CAMetalLayer and starts the frame loop.
///
/// Returns NO and sets `lastError` rather than failing silently: a blank viewport and a broken
/// renderer must never look the same. That rule is inherited from the desktop shell, where "why is
/// it blank" was twice "bgfx fell back to Noop".
- (BOOL)start;

/// Which renderer bgfx actually chose — "Metal", or "Noop" when something went wrong.
@property(nonatomic, readonly, copy) NSString *rendererName;

/// The reason `start` failed, or an empty string.
@property(nonatomic, readonly, copy) NSString *lastError;

/// Whether the renderer is up and presenting to this view's layer.
///
/// A shell MUST show something different when this is NO. A viewport that failed to initialise and
/// a viewport that initialised and drew nothing look identical on screen, and telling them apart
/// after the fact has cost days on the desktop side — twice, both times "bgfx fell back to Noop".
@property(nonatomic, readonly) BOOL attached;

/// What the renderer thinks it is drawing: renderer name, whether it presents directly, and the
/// object/instance/triangle counts. Strings, because this is for a person to read.
///
/// Exists because "it does not render" has too many causes to guess between: no surface, no
/// shaders, an empty scene, or a camera pointing away from the model. Each of those shows up
/// differently here.
- (NSDictionary<NSString *, NSString *> *)diagnostics;

/// Invokes a shared-layer command by its stable id ("feature.box", "feature.cylinder", …). The ids
/// come from `app/`'s command catalogue — the same catalogue the ribbon is built from — so the two
/// shells cannot drift apart on what a command does.
- (void)runCommand:(NSString *)commandId;

/// The labels of every command the catalogue offers, in order, paired with their ids. Used to
/// prove the rail is showing real commands rather than a hard-coded list.
- (NSArray<NSDictionary<NSString *, NSString *> *> *)commands;

/// One row per model-tree item: `label`, `kind`, `state`.
- (NSArray<NSDictionary<NSString *, NSString *> *> *)tree;

/// Whether a stylus has been used at any point in this session.
///
/// # Stylus-preferred, not stylus-only
///
/// The pen draws and the hand navigates — that is the rule, and it needs no modes and no timers.
/// But an iPad with no stylus, or an Android tablet whose stylus does not report as one, would then
/// have NO way to sketch at all, which is a dead end rather than a degraded experience.
///
/// So: once a stylus has touched the screen, a finger never draws again — the user has an
/// instrument and the roles are fixed. Until then, inside the sketch environment, one finger draws
/// and two navigate. The rule is still "what is touching the screen", never "for how long".
@property(nonatomic, readonly) BOOL stylusSeen;

/// Whether a sketch is open, so the shell can show the sketch chrome.
@property(nonatomic, readonly) BOOL sketching;

/// Whether the model is waiting for a plane or face to be chosen for a new sketch.
@property(nonatomic, readonly) BOOL awaitingSketchPlane;

/// Presses Start Sketch. With a face or plane selected it opens there; with nothing selected it
/// asks, and `awaitingSketchPlane` becomes true until a tap answers.
- (void)startSketch;

/// Starts a sketch on the face or plane under a point, in POINTS.
///
/// The order every CAD application uses: pick the surface, then draw on it. Returns NO and reports
/// why through `onStatus` when the target is not something a sketch can sit on — a curved face, an
/// edge, or empty space.
/// Takes a POINT rather than two coordinates so Swift imports it as `beginSketch(at:)`. The
/// two-argument form arrives as `beginSketchAtX(_:y:)`, which reads like a typo at every call site.
- (BOOL)beginSketchAt:(CGPoint)point;

/// Chooses the drawing tool while a sketch is open: "line", "circle", "rectangle", "trim" or
/// "dimension".
///
/// Strings rather than an enum, for the same reason commands are addressed by id: the shell should
/// not carry a second copy of a vocabulary the shared layer already owns.
- (void)setSketchTool:(NSString *)tool;

/// Which tool is active, or an empty string outside a sketch.
@property(nonatomic, readonly, copy) NSString *sketchTool;

/// Which constraints the current sketch selection can take: "horizontal", "vertical", "parallel",
/// "perpendicular", "equal", "tangent".
///
/// The list is the MODEL's, so the rail cannot offer something that would then be refused — the
/// menu adapts to the selection, which is what stops it being a wall of buttons that mostly produce
/// error messages.
- (NSArray<NSString *> *)applicableConstraints;

/// Applies one by name. Returns NO when it did not apply; the reason arrives through `onStatus`.
- (BOOL)applyConstraint:(NSString *)name;

/// How many sketch curves are selected, so the shell can prompt for more.
@property(nonatomic, readonly) NSInteger sketchSelectionCount;

/// Every dimension in the sketch, positioned: `x`, `y` in VIEW POINTS, `text`, and `preview`
/// ("1" while the shape is still being drawn).
///
/// Placed by the model, which owns the camera and the sketch's frame; the shell only draws. That is
/// what puts the width along the bottom of a rectangle and the height up its side on both shells
/// without either one working out where.
- (NSArray<NSDictionary<NSString *, NSString *> *> *)dimensionLabels;

/// The dimension waiting to be typed into, and what it currently reads.
///
/// A desktop takes the number from the keyboard that is already there; a tablet has to put a field
/// on screen, so the shell needs to know when to show one and what to seed it with. Empty when no
/// dimension is being edited.
@property(nonatomic, readonly, copy) NSString *pendingDimension;

/// Applies a typed dimension, in the document's display units. Returns NO if the text is not a
/// length, leaving the field for the user to correct rather than discarding what they typed.
- (BOOL)commitDimension:(NSString *)text;

/// Abandons the dimension being typed, leaving it at the value the geometry already had.
- (void)cancelDimension;

- (void)finishSketch;
- (void)cancelSketch;

/// Tears down the renderer and the document, deterministically.
///
/// **bgfx is a process-wide singleton.** One context, one `bgfx::init`. So a viewport that is still
/// alive when the next one starts does not merely waste memory — it makes the next `attachRenderer`
/// FAIL, which is what "the renderer could not start" means when it appears on reopening a project
/// that rendered perfectly the first time.
///
/// Relying on `dealloc` is not good enough for that, and here it did not happen at all: the shell's
/// callback blocks capture SwiftUI state that holds this view, so view → block → state → view is a
/// retain cycle and `dealloc` never runs. Clearing the blocks is what breaks it, and doing so from
/// an explicit teardown means the timing is a fact rather than a hope.
- (void)shutdown;

/// A tap selects what is under the finger.
///
/// The finger's radius is decided HERE rather than by the shared layer, because it is the one thing
/// that genuinely differs between a mouse and a hand: Apple's minimum touch target is 44 points, so
/// that is the aperture a tap passes, while the desktop passes a few pixels. Everything after that
/// — ranking the candidates, honouring the selection level, toggling an already-selected element —
/// is `Controller::tapAt`, shared. See docs/design/SELECTION.md.
- (void)tapAtX:(CGFloat)x y:(CGFloat)y;

/// A double tap selects the whole BODY under the point.
///
/// The tablet convention, and the reason a single tap can be finer-grained: one tap takes the
/// vertex, edge or face you touched; two take the part it belongs to. Without the second gesture a
/// tablet needs a persistent selection-filter control, which is the desktop's answer and costs
/// screen a tablet cannot spare.
- (void)doubleTapAtX:(CGFloat)x y:(CGFloat)y;

/// Everything under a point, best first: `label`, `slot`, `kind`.
///
/// The list behind a Select Other / precision selector UI. Returned even when nothing needs it yet,
/// because it is the SAME query the tap makes — taking the first entry is a shortcut, not a
/// different code path.
- (NSArray<NSDictionary<NSString *, NSString *> *> *)candidatesAtX:(CGFloat)x y:(CGFloat)y;

/// Which document objects are selected, by label. For the Items panel.
- (NSArray<NSString *> *)selectionLabels;

/// Frames the model.
///
/// Named `fitCamera` and not `fitView`: Swift's Objective-C importer treats a `…View` suffix on a
/// method of a UIView subclass as noise and renames it to `fit()`, which is a compile error at
/// every call site written against the declared name.
- (void)fitCamera;

/// Reads and writes the native document format (ADR 0003). Returns NO on failure.
- (BOOL)openDocumentAtPath:(NSString *)path;
- (BOOL)saveDocumentAtPath:(NSString *)path;

/// Called when the renderer comes up, or fails to.
///
/// A CALLBACK and not just the `attached` property, because the property alone is unobservable from
/// SwiftUI: a body that reads it renders once, before the view has been laid out and therefore
/// before the renderer can possibly have started, and nothing re-evaluates it afterwards. The
/// failure banner then sits over a perfectly live viewport until some unrelated state change
/// happens to redraw the screen.
///
/// Which is worse than the bug it was added to catch: it makes a working renderer look broken.
@property(nonatomic, copy, nullable) void (^onStarted)(BOOL ok, NSString *error);

/// Offered every tap BEFORE selection sees it, in view points. Return YES to consume it.
///
/// The seam that lets "the next tap chooses a sketch plane" live in the shell, where the button
/// that started it lives, rather than as a mode flag inside this class. A bridge that knew about
/// pending UI intentions would be a second place where interaction state lives.
@property(nonatomic, copy, nullable) BOOL (^onTap)(CGFloat x, CGFloat y);

/// The sketch's dimension labels, pushed when they change.
///
/// Delivered rather than fetched: a shell that asked for them from inside a model notification would
/// be re-entering the Controller part-way through its own work. Fired from the frame tick, which is
/// outside all of that.
@property(nonatomic, copy, nullable) void (^onDimensions)(NSArray<NSDictionary<NSString *, NSString *> *> *);

/// Called after the document changes, so the shell can refresh its tree without polling.
@property(nonatomic, copy, nullable) void (^onDocumentChanged)(void);

/// Status text meant for a person — never a stack trace.
@property(nonatomic, copy, nullable) void (^onStatus)(NSString *);

@end

NS_ASSUME_NONNULL_END
