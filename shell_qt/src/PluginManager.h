#pragma once

/// The plugin manager window.
///
/// Lists what is installed, what each plugin claims, and what this host would do with it. It does
/// NOT load anything: opening this window must not execute a plugin, which is the same rule the
/// manifest exists to make possible. A manager that loaded plugins in order to describe them would
/// mean "let me see what I have installed" is a request to run all of it.
///
/// A window rather than a dock, because managing plugins is a task you finish and close, not a
/// panel you work beside — and because it is reachable from Home, where there is no document to
/// dock against.

#include <QDialog>

class QTableWidget;
class QLabel;

namespace cadqt {

class PluginManager : public QDialog {
    Q_OBJECT
public:
    explicit PluginManager(QWidget* parent = nullptr);

private:
    void refresh();

    QTableWidget* table_ = nullptr;
    QLabel* location_ = nullptr;
    QLabel* trust_ = nullptr;
};

}  // namespace cadqt
