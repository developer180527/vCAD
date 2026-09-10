#include "proshell/WindowChrome.h"

#include <QColor>
#include <QGuiApplication>
#include <QWidget>
#include <QWindow>

#import <AppKit/AppKit.h>

namespace proshell {
namespace {

/// The NSWindow behind a QWidget, or nil if it has not been created yet.
///
/// `winId()` on a Qt macOS window handle is the NSView, not the NSWindow -- a detail worth stating
/// because casting it to the wrong one compiles perfectly and then sends messages into a view.
NSWindow* nativeWindow(const QWidget* widget) {
    if (widget == nullptr) return nil;
    // COCOA ONLY. Under the offscreen and minimal platform plugins `winId()` still returns a value,
    // and it is not an NSView: sending it `-window` is objc_msgSend on garbage, which is how the
    // shell probe came to exit 139 before printing a line. Viewport.cpp guards its own winId() for
    // the same reason; this is the same rule, in the one place every call here goes through.
    if (QGuiApplication::platformName() != QStringLiteral("cocoa")) return nil;
    QWindow* handle = widget->windowHandle();
    if (handle == nullptr) return nil;
    auto* view = reinterpret_cast<NSView*>(handle->winId());   // NOLINT(performance-no-int-to-ptr)
    return view != nil ? [view window] : nil;
}

/// What the traffic lights measured on macOS 15, used only until a real window can be asked.
constexpr int kAssumedInset = 78;

}  // namespace

bool hasSystemWindowButtons() { return true; }

void adoptTitleBar(QWidget* window, const QColor& chrome) {
    if (window == nullptr) return;
    if (QGuiApplication::platformName() != QStringLiteral("cocoa")) return;

    // THROUGH QT, not around it. Qt owns this NSWindow's style mask and lays its content view out
    // itself, so setting NSWindowStyleMaskFullSizeContentView directly was quietly undone: the probe
    // measured the traffic lights at y -23..-9 against a strip starting at 0 -- the buttons in a band
    // of their own ABOVE the strip, the one outcome this chrome exists to prevent, and invisible to
    // any screenshot because the buttons are not widgets. ExpandedClientAreaHint is Qt's supported
    // way to say "lay the client area under the title bar", and NoTitleBarBackgroundHint stops the
    // bar painting its own material over it.
    //
    // Only when missing. Changing a window's flags re-creates its native window, which hides it --
    // harmless before the first show, but this is also called on every theme change.
    const Qt::WindowFlags wanted = Qt::ExpandedClientAreaHint | Qt::NoTitleBarBackgroundHint;
    if ((window->windowFlags() & wanted) != wanted) window->setWindowFlags(window->windowFlags() | wanted);

    // AND stop the contents stepping back out of it. Qt 6.9 pairs that hint with safe-area margins,
    // and a top-level widget honours them by default -- so the client area extended under the bar
    // and the window's own contents margin pushed everything straight back down by the bar's height.
    // Measured: buttons at y 9..23, strip at 32..60, and a blank band where the title had been.
    // The strip leaves room for the buttons itself, horizontally, which is the only room they need.
    window->setAttribute(Qt::WA_ContentsMarginsRespectsSafeArea, false);

    NSWindow* native = nativeWindow(window);
    if (native == nil) return;

    // The title text, which would otherwise sit on top of the File button. The traffic lights are
    // NOT touched: they stay where the system puts them and keep every behaviour they have --
    // including greying minimise out in fullscreen, a rule this application would otherwise have to
    // know and would get wrong the first time Apple changed it.
    native.titleVisibility = NSWindowTitleHidden;

    // Whatever band the system still draws takes the CHROME's colour, not the system's default, so
    // the strip and anything above it read as one continuous piece of chrome.
    //
    // Passed in rather than written here. It was a literal -- Paper White's chrome, restated in
    // AppKit terms -- which would have stayed Paper White through every other theme.
    native.backgroundColor = [NSColor colorWithSRGBRed:chrome.redF()
                                                 green:chrome.greenF()
                                                  blue:chrome.blueF()
                                                 alpha:1.0];

    // The APPLICATION's appearance, not the system's, because that band is drawn by the system and
    // the strip below it by us. A Mac in dark mode drew it near-black above light chrome -- the
    // "traffic lights sit in a dark band" report, a theme mismatch rather than a layout fault.
    // Decided by the chrome's own lightness, so a dark theme gets a dark band rather than the Aqua
    // one this used to pin unconditionally.
    native.appearance = [NSAppearance
        appearanceNamed:chrome.lightness() >= 128 ? NSAppearanceNameAqua : NSAppearanceNameDarkAqua];

}

bool titleBarDoubleClicked(QWidget* window) {
    NSWindow* native = nativeWindow(window);
    if (native == nil) return false;

    // What System Settings > Desktop & Dock > "Double-click a window's title bar to" says. Read, not
    // assumed: this Mac is set to "Fill", and a strip that always maximised would have overridden
    // the user's own choice on exactly the gesture that setting governs.
    NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
    NSString* action = [defaults stringForKey:@"AppleActionOnDoubleClick"];
    if (action == nil && [defaults boolForKey:@"AppleMiniaturizeOnDoubleClick"]) action = @"Minimize";

    if ([action isEqualToString:@"None"]) return true;   // handled: the user asked for nothing
    if ([action isEqualToString:@"Minimize"]) {
        [native performMiniaturize:nil];
        return true;
    }
    // "Maximize", "Fill", unset, and anything a later macOS adds. Fill is the system's own tiling and
    // has no public API; zoom is the closest behaviour a window can ask for, and the right fallback
    // for a value this code has never seen.
    [native performZoom:nil];
    return true;
}

SystemButtonBand systemButtonBand(const QWidget* window) {
    NSWindow* native = nativeWindow(window);
    if (native == nil) return {};
    NSButton* close = [native standardWindowButton:NSWindowCloseButton];
    if (close == nil) return {};

    // Converted into the CONTENT VIEW, which is the widget's own coordinate space.
    //
    // Flipped only if that view is NOT already flipped. AppKit measures up from the bottom by
    // default, but Qt's NSView is a flipped view that measures down from the top like every widget,
    // and `convertRect:toView:` already answers in the destination's own orientation. Flipping
    // unconditionally put the traffic lights at y 809 of an 837-pixel window -- the bottom edge --
    // and nobody saw it, because until the probe asked, nothing had ever called this.
    NSView* content = native.contentView;
    const NSRect inView = [[close superview] convertRect:close.frame toView:content];
    const CGFloat top =
        content.isFlipped ? NSMinY(inView) : content.frame.size.height - NSMaxY(inView);
    return {static_cast<int>(top), static_cast<int>(inView.size.height)};
}

int systemButtonInset(const QWidget* window) {
    NSWindow* native = nativeWindow(window);
    if (native == nil) return kAssumedInset;

    NSButton* zoom = [native standardWindowButton:NSWindowZoomButton];
    if (zoom == nil) return kAssumedInset;

    // Trailing gap equal to the leading one, which is what the system's own spacing looks like and
    // keeps the File button from crowding the green light.
    const NSRect frame = zoom.frame;
    return static_cast<int>(NSMaxX(frame) + frame.origin.x);
}

}  // namespace proshell
