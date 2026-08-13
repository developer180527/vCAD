#pragma once

#include "cad/app/Session.h"

#include <QWidget>

class QLineEdit;
class QComboBox;
class QGridLayout;
class QLabel;

namespace cadqt {

/// Inventor's Home, copied rather than reinvented.
///
/// Not a document (ADR 0009): it has no file, cannot be closed, and contributes no ribbon tabs of
/// its own. Modelling it as one would mean every "for each open document" loop carries a special
/// case forever.
///
/// The layout is Inventor's, deliberately and in detail — a left rail carrying product identity,
/// Open/New and help links, and a Recent area with a view toggle, a sort control, a search box
/// and a grid of document cards. This is settled interaction in this industry; a CAD user opening
/// vCAD for the first time should not have to learn where anything is.
///
/// One addition that is ours: the project strip along the top. It surfaces the shared DDC
/// (ADR 0004), which no other CAD application has, so there is nowhere to copy it from.
class HomePage : public QWidget {
    Q_OBJECT
public:
    explicit HomePage(cad::app::Session& session, QWidget* parent = nullptr);
    void refresh();

    /// The left rail.
    ///
    /// Built here but PLACED by MainWindow, because it has to own the full left column — from
    /// under the ribbon down to the status bar, past the document tab bar rather than cut off by
    /// it — and that layout lives a level up. A sidebar that stops short of the window edge reads
    /// as a panel inside the page instead of a rail beside it.
    [[nodiscard]] QWidget* sidebar() noexcept { return sidebar_; }

    /// The rail's starting width. MainWindow needs it to set the splitter's initial sizes; the
    /// min/max the drag respects are set on the widget itself.
    [[nodiscard]] static int sidebarDefaultWidth() noexcept;

signals:
    void createRequested(int kind);
    void openRequested(const QString& path);
    /// The sidebar's Open... button. The shell owns the file dialog, not this page.
    void openBrowseRequested();

private:
    QWidget* buildSidebar();
    QWidget* buildProjectStrip();
    QWidget* buildRecentToolbar();

    /// One recent-document card: thumbnail, kind badge, name, timestamp.
    QWidget* buildCard(const std::filesystem::path&);
    /// Shown instead of the card grid when there is nothing recent. Carries the New tiles, so a
    /// first run has something to do rather than an empty rectangle.
    QWidget* buildEmptyState();

    void rebuildCards();

    cad::app::Session& session_;

    QWidget* sidebar_ = nullptr;

    QLabel* projectName_ = nullptr;
    QLabel* projectPaths_ = nullptr;
    QLabel* cacheStatus_ = nullptr;

    QLineEdit* search_ = nullptr;
    QComboBox* sort_ = nullptr;
    QWidget* cardsHost_ = nullptr;
    QGridLayout* cards_ = nullptr;
    bool gridView_ = true;
};

}  // namespace cadqt
