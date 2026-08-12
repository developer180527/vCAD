#pragma once
#include <QIcon>
#include <QString>

namespace cadqt {

/// Icons drawn from geometry at the requested size, not loaded from files.
///
/// Deliberate: a CAD app needs a coherent icon set, buying or drawing one is a project of its
/// own, and shipping placeholder PNGs means licence questions plus a scaling problem on HiDPI.
/// These are simple and obviously provisional — which is honest about what they are. Replace
/// with a real set before anyone sees a release.
///
/// Rendered at the *current* screen's device pixel ratio. Known limitation: an icon built on a
/// 2x display and then dragged to a 1x monitor is not re-rendered, because these are baked
/// QPixmaps created once. The real fix is a QIconEngine, which re-renders per paint and per
/// ratio; worth doing when the icon set becomes real, not before.
QIcon icon(const QString& name, int size = 32);

}  // namespace cadqt
