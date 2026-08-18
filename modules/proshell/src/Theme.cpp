#include "proshell/Theme.h"

#include <QObject>
#include <QPalette>
#include <QRegularExpression>

#include <algorithm>

namespace proshell {

namespace {

/// Remaps one of Paper White's colours into another theme.
///
/// Written as a TRANSFORM of the reference rather than as 29 hand-picked values per theme, for two
/// reasons. Paper White then stays the literal template — its own output is unchanged by
/// construction, which is what "frozen" has to mean if it is to survive anyone editing this file.
/// And a transform keeps the relationships that make a theme coherent: the hairline stays one step
/// from the chrome, hover stays one step from rest, and disabled stays the same distance from text
/// in all four.
///
/// Neutrals are remapped by LIGHTNESS; anything saturated keeps its hue and saturation, because the
/// accent blue is an identity and a dark theme with a different-coloured selection reads as a
/// different application.
QColor remap(const QColor& from, Theme theme) {
    int h = 0, sat = 0, light = 0;
    from.getHsl(&h, &sat, &light);

    // Saturated: an accent. Keep the hue, nudge lightness only enough to stay legible on the new
    // ground -- a dark theme needs its accents slightly brighter, not different.
    const bool accent = sat > 60;

    switch (theme) {
        case Theme::PaperWhite:
            return from;   // the reference, returned untouched

        case Theme::ClassicWhite:
            // Brighter and cooler: push neutrals toward white and drain the warmth out of them.
            if (accent) return QColor::fromHsl(h, sat, light);
            return QColor::fromHsl(h, sat / 3, std::min(255, static_cast<int>(light * 1.06) + 6));

        case Theme::Midnight: {
            // Inverted, then blue-shifted. The hue is forced toward 215 degrees for neutrals, which
            // is what makes it read as blue-black rather than as grey with a tint.
            if (accent) return QColor::fromHsl(h, sat, std::min(235, light + 30));
            // Compressed into 38..208 rather than 18..210. A dark theme whose darkest surface is
            // near-black has nowhere left to put a border, so every edge disappears and the window
            // reads as one flat shape -- which is what the first attempt looked like.
            const int inverted = 255 - light;
            const int floored = 38 + (inverted * (208 - 38)) / 255;
            // Saturation rises as it darkens. A constant saturation reads as grey in the shadows,
            // which is exactly where a blue-black theme has to be blue to be one at all.
            const int blueness = 22 + (208 - floored) / 6;
            return QColor::fromHsl(214, std::min(60, blueness), floored);
        }

        case Theme::ClassicDark: {
            // Inverted and NEUTRAL. The difference from Midnight is only the absence of a hue,
            // which is the whole point of offering both.
            if (accent) return QColor::fromHsl(h, sat, std::min(235, light + 30));
            // Same compression as Midnight, so the two differ ONLY in hue -- which is the whole
            // reason for offering both.
            const int inverted = 255 - light;
            return QColor::fromHsl(h, 0, 40 + (inverted * (210 - 40)) / 255);
        }
    }
    return from;
}

/// Rewrites every #rrggbb in the template. Case-insensitive on the digits because the template is
/// not consistent about it, and a missed literal is a colour that stays light on a dark theme.
QString recolour(QString sheet, Theme theme) {
    if (theme == Theme::PaperWhite) return sheet;

    static const QRegularExpression hex(QStringLiteral("#([0-9a-fA-F]{6})"));
    QString out;
    out.reserve(sheet.size());
    qsizetype at = 0;
    auto matches = hex.globalMatch(sheet);
    while (matches.hasNext()) {
        const auto m = matches.next();
        out.append(sheet.mid(at, m.capturedStart() - at));
        out.append(remap(QColor(m.captured(0)), theme).name(QColor::HexRgb));
        at = m.capturedEnd();
    }
    out.append(sheet.mid(at));
    return out;
}

}  // namespace

QStringList themeNames() {
    // In Theme order, so a settings Choice's index IS the enum value.
    return {QObject::tr("Paper White"), QObject::tr("Classic White"), QObject::tr("Midnight"),
            QObject::tr("Classic Dark")};
}

void applyTheme(QApplication& app, Theme theme) {
    app.setStyle(QStringLiteral("Fusion"));

    // LIGHT, because that is what Inventor actually looks like.
    //
    // An earlier version was dark, which was a guess dressed up as a decision — the reference
    // screenshots are Inventor's default light scheme: warm-grey chrome, a pale blue-grey
    // viewport, and a saturated blue for selection. The viewport being LIGHTER than the chrome is
    // characteristic and worth matching; it makes the model the brightest thing on screen.
    const QColor chrome = remap(QColor(0xf0, 0xef, 0xed), theme);
    // Deliberately the same paper as `chrome`, not a lighter one. A near-white work surface
    // beside a warm-grey ribbon reads as a glaring panel rather than as one continuous sheet,
    // and this theme's whole character is that nothing on it is stark.
    const QColor panel = remap(QColor(0xf0, 0xef, 0xed), theme);
    const QColor line = remap(QColor(0xcf, 0xcd, 0xc9), theme);
    const QColor text = remap(QColor(0x1f, 0x21, 0x24), theme);
    const QColor dim = remap(QColor(0x84, 0x88, 0x8d), theme);
    const QColor accent = remap(QColor(0x0a, 0x6c, 0xc4), theme);

    QPalette p;
    p.setColor(QPalette::Window, chrome);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, panel);
    p.setColor(QPalette::AlternateBase, chrome);
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, chrome);
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::Highlight, accent);
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::ToolTipBase, QColor(0xff, 0xff, 0xe1));
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::Disabled, QPalette::Text, dim);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, dim);
    p.setColor(QPalette::Disabled, QPalette::WindowText, dim);
    app.setPalette(p);

    app.setStyleSheet(recolour(QStringLiteral(R"(
        /* Hairlines between the docks and the workspace. Needed now that the panels and the
           viewport are the same paper: without them the window is one undivided sheet and the
           eye cannot find the edge of the model tree. One device pixel of the theme's own line
           colour, matching the rule under the ribbon. */
        /* Checkboxes. Styled explicitly because they are otherwise INVISIBLE on the light
           themes: once an application stylesheet exists, Qt draws the indicator through the
           stylesheet style, and with no rule for it the box comes out the same paper colour as
           the form behind it. It looked fine while the app was being reviewed in a dark theme,
           which is exactly why it survived -- the two themes disagreed and only one was looked at.

           Filled with the accent when checked rather than carrying a tick glyph: a tick would need
           an image, and proshell paints its icons programmatically with no resource file to point
           `image: url()` at. A filled box is unambiguous; a tick is nicer, and is worth doing when
           there is a resource system to hang it on. */
        QCheckBox::indicator, QGroupBox::indicator {
            width: 13px; height: 13px;
            border: 1px solid #9a9892; border-radius: 2px; background: #ffffff;
        }
        QCheckBox::indicator:hover { border-color: #0a70c8; }
        QCheckBox::indicator:checked {
            background: #0a70c8; border-color: #0a70c8;
        }
        QCheckBox::indicator:disabled { border-color: #c3c1bd; background: #eceae7; }
        QCheckBox::indicator:checked:disabled { background: #b3b1ad; border-color: #b3b1ad; }
        /* The live dimension readout that follows the cursor while sketching. Styled here with
           everything else so it goes through recolour() and the dark themes get it free. */
        #sketchDimension {
            background: #ffffff; color: #1f2124;
            border: 1px solid #0a70c8; border-radius: 3px;
            padding: 2px 6px; font-size: 12px;
        }
        QMainWindow::separator { background: #cfcdc9; width: 1px; height: 1px; }
        QMainWindow::separator:hover { background: #0a6cc4; }
        QWidget { font-size: 12px; color: #1f2124; }

        /* ── quick access toolbar: the strip above the ribbon ──────────────────── */
        #qat { background: #e9e8e5; border-bottom: 1px solid #cfcdc9; }
        #qatButton { border: 1px solid transparent; border-radius: 3px; padding: 3px; }
        #qatButton:hover { background: #d7e5f3; border-color: #a8c7e6; }
        #qatButton:disabled { color: #a8abaf; }

        /* Selection filter: one segmented control, not four loose buttons. The shared border
           and the squared inner corners are what make it read as a single choice. */
        #qatFilterLabel { color: #6c7075; padding-right: 6px; }
        #qatFilter {
            border: 1px solid #c3c1bd; border-left-width: 0; background: #f4f3f1;
            padding: 2px 9px; color: #33373b;
        }
        #qatFilter:first-child { border-left-width: 1px;
                                 border-top-left-radius: 3px; border-bottom-left-radius: 3px; }
        #qatFilter:last-child { border-top-right-radius: 3px; border-bottom-right-radius: 3px; }
        #qatFilter:hover { background: #e4eef8; }
        #qatFilter:checked { background: #0a70c8; color: #ffffff; border-color: #0a70c8; }
        #fileTab {
            background: #0a6cc4; color: #ffffff; border: none;
            padding: 5px 16px; font-weight: 600;
        }
        #fileTab:hover { background: #0b7ce0; }

        /* ── ribbon ────────────────────────────────────────────────────────────── */
        #ribbon { background: #f0efed; border-bottom: 1px solid #cfcdc9; }
        #ribbonTabs { background: #e9e8e5; }
        #ribbonTabs::tab {
            background: transparent; color: #3c4045;
            padding: 5px 14px; border: none; border-bottom: 2px solid transparent;
        }
        #ribbonTabs::tab:selected {
            background: #f0efed; color: #0a6cc4; border-bottom-color: #0a6cc4;
        }
        #ribbonTabs::tab:hover:!selected { background: #dfdedb; }
        #ribbonPages { background: #f0efed; }

        /* The caption under each panel is what reads as "ribbon" not "toolbar". */
        #ribbonPanelCaption { color: #6c7075; font-size: 10px; padding-top: 1px; }
        #ribbonPanelDivider, #ribbonSeparator { color: #d9d7d3; }
        #ribbonLarge, #ribbonSmall {
            border: 1px solid transparent; border-radius: 3px; padding: 3px;
        }
        #ribbonLarge:hover, #ribbonSmall:hover { background: #d7e5f3; border-color: #a8c7e6; }
        #ribbonLarge:pressed, #ribbonSmall:pressed { background: #b9d4ee; }
        #ribbonLarge:disabled, #ribbonSmall:disabled { color: #a8abaf; }
        #ribbonCollapse { color: #6c7075; border: none; padding: 4px 8px; }

        /* ── document tabs along the bottom, as Inventor does ─────────────────── */
        #docTabs { background: #e9e8e5; border-top: 1px solid #cfcdc9; }
        #docTabs::tab {
            background: #dfdedb; color: #3c4045; border: 1px solid #cfcdc9;
            border-bottom: none; padding: 4px 12px; margin-right: 2px;
        }
        #docTabs::tab:selected { background: #fafaf9; color: #0a6cc4; }

        /* ── docks ─────────────────────────────────────────────────────────────── */
        QDockWidget::title {
            background: #e9e8e5; color: #3c4045;
            padding: 5px 8px; border-bottom: 1px solid #cfcdc9;
        }
        QTreeView, QTableView, QListWidget {
            background: #fafaf9; border: none; outline: none;
            selection-background-color: #cde3f7; selection-color: #1f2124;
        }
        QTreeView::item, QTableView::item { padding: 3px 2px; }
        QTreeView::item:hover { background: #eaf2fb; }
        QHeaderView::section {
            background: #f0efed; color: #6c7075; border: none;
            border-bottom: 1px solid #cfcdc9; padding: 4px;
        }
        QStatusBar { background: #e9e8e5; color: #3c4045; }
        QStatusBar::item { border: none; }
        QSplitter::handle { background: #cfcdc9; }
        QLineEdit {
            background: #ffffff; border: 1px solid #cfcdc9; border-radius: 2px;
            padding: 2px 4px;
        }
        QLineEdit:focus { border-color: #0a6cc4; }

        /* ── home page ─────────────────────────────────────────────────────────── */
        #homePage { background: #fafaf9; }
        #homeTitle { font-size: 26px; font-weight: 300; color: #3c4045; }
        #homeSection { font-size: 13px; font-weight: 600; color: #3c4045; }
        #homeRule { color: #dfdedb; }
        #homeTile {
            background: #ffffff; border: 1px solid #cfcdc9; border-radius: 4px; color: #3c4045;
        }
        #homeTile:hover { background: #eaf2fb; border-color: #0a6cc4; }
        #homeTile:disabled { background: #f4f3f1; color: #a8abaf; }
        #homeRecent { background: #fafaf9; }

        /* The browser's badge column. Smaller than the name so the tree still reads as a list of
           features with annotations, rather than two columns of equal weight. */
        QTreeView::item:!selected { }

        /* The selection toolbar floats over the canvas, so it needs its own edge and a solid
           ground -- a translucent one over sketch geometry is unreadable exactly where it sits. */
        #contextBar {
            background: #f6f5f3; border: 1px solid #b8b6b1; border-radius: 4px;
        }

        /* ── command property panel: the left dock while a command runs ─────────── */
        #commandPanel { background: #fafaf9; }
        #commandTitle { font-weight: 600; color: #1f2124; }
        #commandOk, #commandCancel {
            border: 1px solid #cfcdc9; border-radius: 3px; padding: 4px 16px;
            background: #f0efed;
        }
        #commandOk { background: #0a6cc4; color: #ffffff; border-color: #085aa3; }
        #commandOk:hover { background: #0b7ce0; }
        #commandCancel:hover { background: #d7e5f3; border-color: #a8c7e6; }

        /* ── home, Inventor layout ─────────────────────────────────────────────── */
        #homeSidebar { background: #f1f0ee; border-right: 1px solid #dfdedb; }

        /* The rail's own border-right IS the visible divider. The handle only needs to be a
           grab zone next to it, so it takes the page colour rather than the default splitter
           line — two divider lines four pixels apart looks like a mistake. */
        #homeSplitter::handle { background: #fafaf9; }
        #homeSplitter::handle:hover { background: #d7e5f3; }
        #homeSplitter::handle:pressed { background: #a8c7e6; }
        #homeMain { background: #fafaf9; }
        #homeProduct { font-size: 24px; font-weight: 300; color: #33373b; }
        #homeProductSub { font-size: 12px; color: #83878c; }
        #homeSideButton {
            background: #fafaf9; border: 1px solid #c3c1bd; border-radius: 3px;
            padding: 7px 12px; text-align: left; color: #33373b;
        }
        #homeSideButton:hover { background: #e4eef8; border-color: #0a70c8; }
        #homeSideButton::menu-button { border: 0px; width: 18px; }
        #homeLink { color: #0a70c8; }
        #homeLinkDisabled { color: #a8abaf; }

        #homeHeading { font-size: 21px; font-weight: 300; color: #33373b; }
        #homeStripItem { color: #6c7075; }
        #homeStripSeparator { color: #cfcdc9; }
        #homeStripCache { color: #a8abaf; }
        #homeStripCache[online="true"] { color: #1f8a4c; }
        #homeSort { padding: 2px 6px; min-width: 118px; }
        #homeSearch { padding: 3px 8px; border: 1px solid #c3c1bd; border-radius: 3px;
                      background: #ffffff; }
        #homeViewToggle { border: 1px solid transparent; border-radius: 3px; padding: 3px; }
        #homeViewToggle:checked { background: #d7e5f3; border-color: #a8c7e6; }
        #homeScroll { background: transparent; }
        #homeCards { background: transparent; }

        #homeCard { background: #ffffff; border: 1px solid #dfdedb; border-radius: 3px; }
        #homeCard:hover { border-color: #0a70c8; }
        #homeThumb { background: #f1f0ee; border: 1px solid #e6e5e2; }
        #homeCardName { font-weight: 600; color: #33373b; }
        #homeCardWhen { color: #83878c; font-size: 11px; }
        #homeEmpty { color: #83878c; }
    )"), theme));
}

}  // namespace proshell
