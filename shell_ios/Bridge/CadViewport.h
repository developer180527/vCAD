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

/// Called after the document changes, so the shell can refresh its tree without polling.
@property(nonatomic, copy, nullable) void (^onDocumentChanged)(void);

/// Status text meant for a person — never a stack trace.
@property(nonatomic, copy, nullable) void (^onStatus)(NSString *);

@end

NS_ASSUME_NONNULL_END
