#pragma once
#include <QIcon>
#include <QString>

namespace cadqt {

/// Icons drawn from geometry at the requested size, not loaded from files.
///
/// Deliberate: a CAD app needs a coherent icon set, buying or drawing one is a project of its
/// own, and shipping placeholder PNGs means licence questions plus a scaling problem on HiDPI.
/// These are simple, crisp at any size, and obviously provisional — which is honest about what
/// they are. Replace with a real set before anyone sees a release.
QIcon icon(const QString& name, int size = 32);

}  // namespace cadqt
