#pragma once

/// The window's own buttons, brought into the application's top strip.
///
/// # What this is for
///
/// A title bar above a ribbon wastes a whole row of screen on a word the user already knows, and
/// no professional application in this family ships one: Inventor, SolidWorks, Office, VS Code and
/// Chrome all put their first row of commands where the title bar would be. This is the platform
/// half of doing that -- the part that cannot be written once and must be written per system.
///
/// # Why macOS is not "frameless"
///
/// The obvious route is `Qt::FramelessWindowHint` everywhere. On macOS that is the wrong answer: it
/// removes the traffic lights along with the bar, and everything they carry goes with them --
/// dragging, double-click-to-zoom, snapping, the fullscreen animation, the green button's menu, and
/// the rule that minimise greys out in fullscreen. Reimplementing that convincingly is a project,
/// and reimplementing it badly is worse than a title bar.
///
/// What macOS actually offers is a window whose content view extends UNDER the title bar, with the
/// bar itself transparent and its title hidden. The buttons stay exactly where the system puts them
/// and keep every behaviour they have; the application simply draws behind them, and leaves them
/// room. That is what `adoptTitleBar` does there.
///
/// Elsewhere there is nothing to keep, so the window really does go frameless and the strip carries
/// buttons this library draws. `hasSystemWindowButtons` is the question a caller asks to know which
/// of the two it is looking at, and `systemButtonInset` is how much room to leave for buttons it
/// does not own.

class QColor;
class QWidget;

namespace proshell {

/// Does the system draw window buttons that we host rather than replace?
///
/// True on macOS. A caller uses this to decide whether to draw its own set, NOT to decide where to
/// put them: which side they sit on is a platform convention in its own right (left on macOS, right
/// on Windows and most Linux desktops) and belongs with the code that lays the strip out.
[[nodiscard]] bool hasSystemWindowButtons();

/// Merges the title bar into the window, painting whatever band the system still draws in `chrome`.
///
/// Call once the widget has a native window behind it -- after `show()`, or after touching
/// `winId()`. Before that there is nothing to talk to and this does nothing, silently, because a
/// window that has not been created yet is a timing question rather than an error. Also a no-op
/// under a platform plugin with no native windows (offscreen, minimal).
///
/// `chrome` rather than a colour known here, so the band follows the application's theme -- and is
/// dark under a dark one -- instead of restating one theme's value in AppKit terms. Safe to call
/// again when the theme changes.
void adoptTitleBar(QWidget* window, const QColor& chrome);

/// Does what the platform does when its title bar is double clicked. Returns false where there is
/// no platform rule to follow, and the caller should apply its own.
///
/// On macOS that rule is the USER's: System Settings chooses between filling, zooming, minimising
/// and nothing. Keeping the system's title bar was chosen precisely so behaviours like this one are
/// inherited rather than reinvented, so the strip that replaces the bar has to honour it too.
[[nodiscard]] bool titleBarDoubleClicked(QWidget* window);

/// How much room the system's own buttons need at the leading edge of the strip.
///
/// Measured from the window when there is one, rather than hard-coded: the traffic lights are not
/// a fixed size across macOS versions, and a guessed inset is a number that is right until the year
/// it is not. Falls back to a measured-today default when asked before the window exists.
[[nodiscard]] int systemButtonInset(const QWidget* window);

/// Where the system's own buttons sit, in the widget's own coordinates: the gap above them and the
/// height they occupy. Both zero where there are none.
///
/// Exists to be checked from the outside. Whether the buttons and the strip ended up in the same
/// band is the entire question this chrome has to get right, and no widget screenshot can answer it
/// -- the buttons are not widgets, so `QWidget::grab` renders a window that looks finished whether
/// they landed on the strip or in a band of their own above it.
struct SystemButtonBand {
    int top = 0;
    int height = 0;
};
[[nodiscard]] SystemButtonBand systemButtonBand(const QWidget* window);

}  // namespace proshell
