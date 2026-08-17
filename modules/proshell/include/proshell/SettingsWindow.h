#pragma once

/// The settings window: a page list beside a scrolling form.
///
/// Settled interaction, copied rather than invented — Inventor, Fusion, SolidWorks and every IDE
/// use this shape, and a user who has seen one has seen this. The only thing worth deciding here is
/// what NOT to do: no OK/Cancel.
///
/// # Applied immediately, not on OK
///
/// A settings dialog with OK/Cancel has to buffer every edit and decide what a half-finished form
/// means when the user closes the window some other way. Applying on change removes the question,
/// and it is what makes a live preference — a theme, a navigation preset — feel like a setting
/// rather than a form submission. Reset is the undo, per field or per page.
///
/// This is the same reasoning that made `Settings::reset` remove a key rather than write a default.

#include <QDialog>

#include <QHash>

class QListWidget;
class QStackedWidget;
class QString;

namespace proshell {

class Settings;

class SettingsWindow : public QDialog {
    Q_OBJECT
public:
    /// Renders whatever pages the store holds AT CONSTRUCTION. A store that gains a page later
    /// needs a new window, which is honest: a plugin loaded while the settings window is open is a
    /// case worth not pretending to handle correctly.
    explicit SettingsWindow(Settings& settings, QWidget* parent = nullptr);

    /// Selects a page by id, so an application can open settings AT the relevant page — "configure
    /// this" from a context menu is worth more than "open settings and go looking".
    void showPage(const QString& pageId);

private:
    void buildPage(const class SettingsPage& page);

    Settings& settings_;
    QListWidget* list_ = nullptr;
    QStackedWidget* pages_ = nullptr;
    QHash<QString, int> indexOfPage_;
};

}  // namespace proshell
