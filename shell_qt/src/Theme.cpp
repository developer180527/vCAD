#include "Theme.h"

#include <QPalette>

namespace cadqt {

void applyTheme(QApplication& app) {
    app.setStyle(QStringLiteral("Fusion"));

    // Inventor's dark scheme: near-neutral greys with a cool cast, and a saturated blue for
    // selection. Deliberately low-contrast for panels so the viewport is the brightest thing on
    // screen — the model should draw the eye, not the chrome.
    const QColor base(0x2b, 0x2d, 0x30);
    const QColor panel(0x33, 0x36, 0x3a);
    const QColor raised(0x3c, 0x40, 0x45);
    const QColor text(0xdc, 0xdf, 0xe4);
    const QColor dim(0x8b, 0x91, 0x9a);
    const QColor accent(0x2f, 0x7f, 0xd1);

    QPalette p;
    p.setColor(QPalette::Window, panel);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, base);
    p.setColor(QPalette::AlternateBase, panel);
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, raised);
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::Highlight, accent);
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::ToolTipBase, raised);
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::Disabled, QPalette::Text, dim);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, dim);
    p.setColor(QPalette::Disabled, QPalette::WindowText, dim);
    app.setPalette(p);

    app.setStyleSheet(QStringLiteral(R"(
        QWidget { font-size: 12px; }

        /* ── ribbon ─────────────────────────────────────────────────────────────── */
        #ribbon { background: #33363a; border-bottom: 1px solid #23252a; }
        #ribbonTabs { background: #2b2d30; }
        #ribbonTabs::tab {
            background: #2b2d30; color: #8b919a;
            padding: 6px 16px; border: none; margin-right: 1px;
        }
        #ribbonTabs::tab:selected { background: #33363a; color: #dcdfe4; }
        #ribbonTabs::tab:hover:!selected { color: #dcdfe4; }
        #ribbonPages { background: #33363a; }

        /* The caption under each panel is what reads as "ribbon" rather than "toolbar". */
        #ribbonPanelCaption { color: #767d87; font-size: 10px; padding-top: 1px; }
        #ribbonPanelDivider, #ribbonSeparator { color: #43474d; }

        #ribbonLarge, #ribbonSmall {
            color: #dcdfe4; border: 1px solid transparent; border-radius: 3px; padding: 3px;
        }
        #ribbonLarge:hover, #ribbonSmall:hover { background: #43474d; border-color: #52575e; }
        #ribbonLarge:pressed, #ribbonSmall:pressed { background: #2f7fd1; }
        #ribbonLarge:disabled, #ribbonSmall:disabled { color: #5d636b; }
        #ribbonCollapse { color: #8b919a; border: none; padding: 4px 8px; }
        #ribbonCollapse:hover { color: #dcdfe4; }

        /* ── docks ──────────────────────────────────────────────────────────────── */
        QDockWidget { titlebar-close-icon: none; titlebar-normal-icon: none; }
        QDockWidget::title {
            background: #2b2d30; color: #8b919a;
            padding: 5px 8px; border-bottom: 1px solid #23252a;
        }
        QTreeView, QTableView {
            background: #2b2d30; border: none; outline: none;
            selection-background-color: #2f7fd1;
        }
        QTreeView::item, QTableView::item { padding: 3px 2px; }
        QTreeView::item:hover { background: #383b40; }
        QHeaderView::section {
            background: #33363a; color: #8b919a; border: none;
            border-bottom: 1px solid #23252a; padding: 4px;
        }

        QStatusBar { background: #23252a; color: #8b919a; }
        QStatusBar::item { border: none; }
        QSplitter::handle { background: #23252a; }
        QScrollBar:vertical { background: #2b2d30; width: 10px; }
        QScrollBar::handle:vertical { background: #4a4f56; border-radius: 5px; min-height: 20px; }
        QScrollBar::add-line, QScrollBar::sub-line { height: 0; width: 0; }
        QLineEdit {
            background: #23252a; border: 1px solid #43474d; border-radius: 2px;
            padding: 2px 4px; color: #dcdfe4;
        }
        QLineEdit:focus { border-color: #2f7fd1; }
    )"));
}

}  // namespace cadqt
