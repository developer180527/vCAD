# 0008 — Qt shell: Inventor-like, and what that actually means

Status: proposed (Aug 2026)

## Context

The goal stated plainly: *"a better FreeCAD"*, looking like Autodesk Inventor.

Both halves matter, and the second is not decoration. FreeCAD's capability is not really the
problem — its interface is what stops people using it. So "looks like Inventor" is shorthand for
a set of interaction decisions, and it is worth naming them rather than chasing a colour scheme.

## What actually makes Inventor feel like Inventor

In rough order of how much it matters:

1. **A ribbon, not toolbars.** Tabbed command groups with large labelled buttons. This is the
   single biggest visual and behavioural signature, and Qt has no ribbon widget — we build it.
   Toolbars are why FreeCAD looks like 2006.
2. **One model browser, always present, showing the feature tree** with per-feature state. Not a
   panel you go and find.
3. **No workbench switcher.** FreeCAD's workbenches are its worst discoverability problem: the
   command you want exists, in a mode you have not selected, and nothing tells you which. A
   ribbon's contextual tabs solve the same problem — grouping commands — without making the user
   change modes first.
4. **Non-modal everything.** FreeCAD's task dialogs block the document while open. Inventor edits
   in place with a floating mini-toolbar. This is an architectural constraint on us, not a
   styling one: the controller must accept edits while a command is in progress.
5. **ViewCube and a navigation bar** in the viewport corner. Small, and the first thing a CAD user
   reaches for.
6. **A status bar that says something** — active units, selection, coordinates.

## Decisions

**Build a ribbon widget.** ~400 lines: a tab bar over stacked pages, each page a row of panels,
each panel a group of large tool buttons with a caption. Qt Widgets gives us the docking, the
actions and the shortcuts; the ribbon is the one thing it does not.

**Qt Widgets, not QML.** Already settled in [ADR 0001](0001-qt-linking.md) and worth repeating:
CAD is dense, keyboard-driven and dockable. QML would mean rebuilding every CAD idiom.

**A UI-agnostic `app/` layer between the shell and the core.** The shell must contain no
modelling logic — not for purity, but because the iPad shell is SwiftUI and will need the same
controller. If a rule about which feature can follow which lives in `MainWindow`, it does not
exist on iPad.

So: `shell_qt/` holds only Qt. `app/` holds the document, selection, commands and undo, and knows
nothing about widgets.

**Dark theme by default**, matching current Inventor. Applied as a stylesheet plus a palette
rather than a custom style class — cheaper, and it keeps native menus and dialogs.

## Qt version: developing on 6.11, shipping on 6.8 LTS

Homebrew ships 6.11.1 and it is already installed, so local development costs nothing. ADR 0001
targets **6.8 LTS** for shipping, because LTS is what gets five years of patches.

That gap is a real hazard: an API introduced in 6.9 will compile here and fail on the shipping
target. Mitigation is discipline plus CI — the Linux and Windows jobs should pin 6.8 so drift
fails there rather than at release. Until CI has Qt, treat "is this API older than 6.8?" as a
review question.

Deliberately NOT using vcpkg for Qt: `qtbase` alone is a multi-hour build with 34 default
features, and it would dominate every cold CI run and every new checkout.

## Consequences

- The viewport starts as a placeholder widget. Wiring the bgfx backend into a `QWidget` with a
  native handle is a separate step, and given how M3.3 went, doing it against a UI we can already
  see is much better than the reverse.
- A ribbon is code we own forever. Accepted: it is the difference between looking like Inventor
  and looking like FreeCAD, which is the entire point of this ADR.
