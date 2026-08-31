#pragma once

/// The parameter table: `width = 40mm`, `wall = width / 8`.
///
/// Modeless, as Fusion's and SolidWorks' both are. That is not a styling choice -- the reason to
/// open this is to change a number and watch the model move, and a modal dialog covering the
/// viewport makes exactly that impossible.
///
/// Every rule about what a parameter may be lives in the Controller and below it. This window
/// knows how to draw a table and which strings to hand over; if it starts deciding what a valid
/// name is, the iPad will need a second copy of that decision.

#include <QDialog>

class QTableWidget;
class QLabel;

namespace cad::app {
class Controller;
}

namespace cadqt {

class ParametersDialog : public QDialog {
    Q_OBJECT

public:
    ParametersDialog(cad::app::Controller& controller, QWidget* parent);

    /// Repopulates from the document. Called when the model changes underneath -- an undo, or a
    /// parameter edited from somewhere else -- so the table can never show a stale number.
    void refresh();

signals:
    /// A parameter changed, so the viewport and the browser need rebuilding.
    void documentChanged();

private:
    void addRow();
    void deleteSelected();
    void commitCell(int row, int column);

    cad::app::Controller& controller_;
    QTableWidget* table_ = nullptr;
    QLabel* problem_ = nullptr;
    bool populating_ = false;   ///< suppresses cell signals while refresh() writes the table
};

}  // namespace cadqt
