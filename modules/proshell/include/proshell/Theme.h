#pragma once
#include <QApplication>
#include <QString>

namespace proshell {

/// Dark palette + stylesheet, matching current Inventor.
///
/// A stylesheet rather than a QStyle subclass: far less code, and native menus and file dialogs
/// keep working. Icons are drawn from geometry (see Icons.h) rather than shipped as assets, so
/// there is nothing to license, scale or forget to install.
void applyTheme(QApplication&);

}  // namespace proshell
