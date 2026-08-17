#pragma once
#include <QApplication>
#include <QString>
#include <QStringList>

namespace proshell {

/// Which colour scheme to paint the shell in.
///
/// `PaperWhite` is the reference and is FROZEN. It is not a starting point to be improved: its
/// warm-grey chrome and identical work surfaces were tuned deliberately, and the whole character of
/// it is that nothing is stark. The other three exist BESIDE it, never by editing it — which is why
/// the implementation keeps Paper White's stylesheet as the literal template and derives the others
/// from it, so its output is byte-identical by construction rather than by care.
enum class Theme {
    PaperWhite = 0,    ///< the default and the reference: warm grey, nothing stark
    ClassicWhite = 1,  ///< brighter and cooler, closer to a default Windows application
    Midnight = 2,      ///< dark, blue-shifted
    ClassicDark = 3,   ///< dark, neutral grey
};

/// Palette + stylesheet, matching current Inventor.
///
/// A stylesheet rather than a QStyle subclass: far less code, and native menus and file dialogs
/// keep working. Icons are drawn from geometry (see Icons.h) rather than shipped as assets, so
/// there is nothing to license, scale or forget to install.
///
/// Safe to call again to switch theme at runtime: it replaces the palette and stylesheet wholesale.
void applyTheme(QApplication&, Theme = Theme::PaperWhite);

/// The user-facing names, in `Theme` order, so a settings Choice's INDEX is the enum value and no
/// mapping table has to be kept in step.
[[nodiscard]] QStringList themeNames();

}  // namespace proshell
