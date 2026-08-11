# 0001 — Qt is dynamically linked and bundled; desktop only

Status: accepted (Aug 2026)

## Decision
Qt **6.8 LTS**, dynamically linked, shipped inside our own installers. Used only in `shell_qt/`.

## Context
- Qt 6.11 is current (2026-03-23); 6.10 supported to 2026-10-07. Neither is LTS.
  Nearest LTS are 6.8 and 6.5, and LTS patch releases go commercial-only after the initial
  open-source phase. We take 6.8 LTS and accept that later patches may need a commercial seat.
- LGPLv3 does **not** require the user to pre-install Qt. It requires that the user *could*
  replace our Qt with their own. Shipping Qt shared libraries inside our bundle satisfies
  this: `macdeployqt` + rpath `@executable_path/../Frameworks`, `windeployqt` + DLLs beside
  the exe, `$ORIGIN/../lib` in the AppImage.
- Static linking would trigger the object-file distribution obligation. We do not do it.

## Consequence for iPad
Qt on iOS is effectively static-only, and static LGPLv3 inside an App Store binary is legally
contested (the signing arguably defeats the relink right). **We do not use Qt on iPad.**
The iPad shell is native SwiftUI/UIKit + Metal over the same `core`. This is also the better
product decision: Apple Pencil needs raw `UITouch` with `coalescedTouches`/`predictedTouches`,
hover, azimuth/altitude/force, and Pencil Pro squeeze/roll — none of which a cross-platform
widget toolkit exposes well.
