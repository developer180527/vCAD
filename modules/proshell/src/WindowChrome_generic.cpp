#include "proshell/WindowChrome.h"

#include <QWidget>

namespace proshell {

/// Windows and the Linux desktops: nothing to keep, so the strip draws its own buttons.
bool hasSystemWindowButtons() { return false; }

void adoptTitleBar(QWidget* window, const QColor&) {
    if (window == nullptr) return;
    // Only when not already set. adoptTitleBar is called again on every theme change, and changing
    // the flags of a window that is on screen re-creates it -- which hides it.
    if (window->windowFlags().testFlag(Qt::FramelessWindowHint)) return;
    // Genuinely frameless here, unlike macOS. There is no equivalent of a transparent title bar
    // with the system's buttons still in it on either Windows or the common Linux compositors, so
    // the bar goes and the strip carries buttons this library draws.
    //
    // Dragging and resizing are not reimplemented from mouse deltas: `startSystemMove` and
    // `startSystemResize` hand the gesture to the compositor, which is what keeps snapping,
    // edge-tiling, shake-to-minimise and multi-monitor DPI changes working. ShellWindow calls both:
    // move from the strip, resize from a few pixels at every edge.
    window->setWindowFlag(Qt::FramelessWindowHint, true);
}

bool titleBarDoubleClicked(QWidget*) { return false; }

int systemButtonInset(const QWidget*) { return 0; }

SystemButtonBand systemButtonBand(const QWidget*) { return {}; }

}  // namespace proshell
