#include "Theme.h"

#include <QPalette>

namespace cadqt {

void applyTheme(QApplication& app) {
    app.setStyle(QStringLiteral("Fusion"));

    // LIGHT, because that is what Inventor actually looks like.
    //
    // An earlier version was dark, which was a guess dressed up as a decision — the reference
    // screenshots are Inventor's default light scheme: warm-grey chrome, a pale blue-grey
    // viewport, and a saturated blue for selection. The viewport being LIGHTER than the chrome is
    // characteristic and worth matching; it makes the model the brightest thing on screen.
    const QColor chrome(0xf0, 0xef, 0xed);
    const QColor panel(0xfa, 0xfa, 0xf9);
    const QColor line(0xcf, 0xcd, 0xc9);
    const QColor text(0x1f, 0x21, 0x24);
    const QColor dim(0x84, 0x88, 0x8d);
    const QColor accent(0x0a, 0x6c, 0xc4);

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

    app.setStyleSheet(QStringLiteral(R"(
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
            background: #0a6cc4; color: white; border: none;
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
            background: white; border: 1px solid #cfcdc9; border-radius: 2px;
            padding: 2px 4px;
        }
        QLineEdit:focus { border-color: #0a6cc4; }

        /* ── home page ─────────────────────────────────────────────────────────── */
        #homePage { background: #fafaf9; }
        #homeTitle { font-size: 26px; font-weight: 300; color: #3c4045; }
        #homeSection { font-size: 13px; font-weight: 600; color: #3c4045; }
        #homeRule { color: #dfdedb; }
        #homeTile {
            background: white; border: 1px solid #cfcdc9; border-radius: 4px; color: #3c4045;
        }
        #homeTile:hover { background: #eaf2fb; border-color: #0a6cc4; }
        #homeTile:disabled { background: #f4f3f1; color: #a8abaf; }
        #homeRecent { background: #fafaf9; }

        /* ── home, Inventor layout ─────────────────────────────────────────────── */
        #homeSidebar { background: #f1f0ee; border-right: 1px solid #dfdedb; }
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
    )"));
}

}  // namespace cadqt
