#pragma once

#include "proshell/HomeModel.h"

#include <QWidget>

class QLineEdit;
class QComboBox;
class QGridLayout;
class QLabel;

namespace proshell {

/// A professional application's Home screen, copied from Inventor rather than reinvented.
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
/// The strip along the top is the one part with no precedent to copy. It exists because vCAD has
/// a shared content-addressed cache to surface (ADR 0004); it is generalised here to a row of
/// `SummaryField`s so an application with something else to say about its workspace has somewhere
/// to say it, and one with nothing simply returns an empty list and the strip disappears.
///
/// Everything the page needs comes through `HomeModel` — see the note there on why THIS one is an
/// interface when the window frame deliberately is not.
class HomePage : public QWidget {
    Q_OBJECT
public:
    /// `model` must outlive the page; it is typically owned by the window.
    explicit HomePage(const HomeModel& model, QWidget* parent = nullptr);
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
    /// `kind` is the application's own `DocumentKind::id`, handed back unchanged.
    void createRequested(int kind);
    void openRequested(const QString& path);
    /// The sidebar's Open... button. The shell owns the file dialog, not this page.
    void openBrowseRequested();

private:
    QWidget* buildSidebar();
    QWidget* buildProjectStrip();
    QWidget* buildRecentToolbar();

    /// One recent-document card: thumbnail, name, timestamp.
    QWidget* buildCard(const RecentDocument&);
    /// Shown instead of the card grid when there is nothing recent. Carries the New tiles, so a
    /// first run has something to do rather than an empty rectangle.
    QWidget* buildEmptyState();

    void rebuildSummary();
    void rebuildCards();

    const HomeModel& model_;

    QWidget* sidebar_ = nullptr;

    /// The summary strip. Rebuilt on every refresh, because the number of fields is the
    /// application's to decide and may change while the page is open.
    QWidget* summaryStrip_ = nullptr;

    QLineEdit* search_ = nullptr;
    QComboBox* sort_ = nullptr;
    QWidget* cardsHost_ = nullptr;
    QGridLayout* cards_ = nullptr;
    bool gridView_ = true;
};

}  // namespace proshell
