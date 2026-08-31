#include "ParametersDialog.h"

#include "cad/app/Controller.h"

#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

namespace cadqt {

namespace {

constexpr int kName = 0;
constexpr int kExpression = 1;
constexpr int kValue = 2;

}   // namespace

ParametersDialog::ParametersDialog(cad::app::Controller& controller, QWidget* parent)
    : QDialog(parent), controller_(controller) {
    setWindowTitle(tr("Parameters"));
    // Modeless: the point is to change a number and watch the model move.
    setModal(false);
    resize(560, 360);

    auto* layout = new QVBoxLayout(this);

    auto* hint = new QLabel(
        tr("Name a value once, then use it anywhere: type <b>width</b> or <b>width / 2</b> into any "
           "field."),
        this);
    hint->setWordWrap(true);
    layout->addWidget(hint);

    table_ = new QTableWidget(0, 3, this);
    table_->setHorizontalHeaderLabels({tr("Name"), tr("Expression"), tr("Value")});
    table_->horizontalHeader()->setSectionResizeMode(kName, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(kExpression, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(kValue, QHeaderView::ResizeToContents);
    table_->verticalHeader()->setVisible(false);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    layout->addWidget(table_, 1);

    // Where a rejected edit is explained. A refused value that says nothing is indistinguishable
    // from a field that did not take the keystroke.
    problem_ = new QLabel(this);
    problem_->setWordWrap(true);
    problem_->setVisible(false);
    layout->addWidget(problem_);

    auto* buttons = new QDialogButtonBox(this);
    auto* add = buttons->addButton(tr("Add"), QDialogButtonBox::ActionRole);
    auto* remove = buttons->addButton(tr("Delete"), QDialogButtonBox::DestructiveRole);
    buttons->addButton(QDialogButtonBox::Close);
    layout->addWidget(buttons);

    connect(add, &QPushButton::clicked, this, &ParametersDialog::addRow);
    connect(remove, &QPushButton::clicked, this, &ParametersDialog::deleteSelected);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
    connect(table_, &QTableWidget::cellChanged, this, &ParametersDialog::commitCell);

    refresh();
}

void ParametersDialog::refresh() {
    // The guard matters: writing cells fires cellChanged, which would commit each row back to the
    // document as it was drawn -- and the FIRST such write, of an empty new row, would report a
    // problem the user never caused.
    populating_ = true;

    const auto rows = controller_.parameters();
    table_->setRowCount(static_cast<int>(rows.size()));
    for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
        const auto& row = rows[static_cast<std::size_t>(i)];

        auto* name = new QTableWidgetItem(QString::fromStdString(row.name));
        table_->setItem(i, kName, name);

        // The FORMULA when there is one, the value when there is not. A field showing the 80 that
        // `width * 2` produced invites the user to retype 80, which quietly breaks the link.
        auto* expression = new QTableWidgetItem(QString::fromStdString(
            row.expression.empty() ? row.value : row.expression));
        table_->setItem(i, kExpression, expression);

        auto* value = new QTableWidgetItem(QString::fromStdString(
            row.problem.empty() ? row.value : tr("—").toStdString()));
        value->setFlags(value->flags() & ~Qt::ItemIsEditable);   // computed, never typed into
        if (!row.problem.empty()) {
            value->setToolTip(QString::fromStdString(row.problem));
            name->setToolTip(QString::fromStdString(row.problem));
        }
        table_->setItem(i, kValue, value);
    }

    // A problem shown here rather than only in a tooltip: a broken parameter that looks fine until
    // hovered is one the user will not find.
    QString problems;
    for (const auto& row : rows) {
        if (row.problem.empty()) continue;
        if (!problems.isEmpty()) problems += QStringLiteral("\n");
        problems += QStringLiteral("%1: %2").arg(QString::fromStdString(row.name),
                                                 QString::fromStdString(row.problem));
    }
    problem_->setText(problems);
    problem_->setVisible(!problems.isEmpty());

    populating_ = false;
}

void ParametersDialog::addRow() {
    // Named for the user rather than asking first: an empty row they can type over is how every
    // spreadsheet-shaped table in this industry behaves, and a modal "what shall it be called?"
    // before they have decided is friction.
    int n = 1;
    std::string name = "parameter1";
    while (!controller_.setParameter(name, "10")) {
        if (++n > 99) return;   // something other than a name clash; give up rather than spin
        name = "parameter" + std::to_string(n);
    }
    emit documentChanged();
    refresh();

    // Straight into editing the name, which is the first thing anyone wants to change.
    for (int i = 0; i < table_->rowCount(); ++i) {
        if (table_->item(i, kName)->text().toStdString() != name) continue;
        table_->setCurrentCell(i, kName);
        table_->editItem(table_->item(i, kName));
        break;
    }
}

void ParametersDialog::deleteSelected() {
    const int row = table_->currentRow();
    if (row < 0 || table_->item(row, kName) == nullptr) return;
    if (controller_.removeParameter(table_->item(row, kName)->text().toStdString())) {
        emit documentChanged();
    }
    refresh();
}

void ParametersDialog::commitCell(int row, int column) {
    if (populating_ || column == kValue) return;

    const auto rows = controller_.parameters();
    if (row < 0 || row >= static_cast<int>(rows.size())) return;
    const auto before = rows[static_cast<std::size_t>(row)];

    bool ok = false;
    if (column == kExpression) {
        ok = controller_.setParameter(before.name, table_->item(row, kExpression)->text()
                                                       .toStdString());
    } else {
        // A rename is add-then-remove, in that order. The reverse would delete the old name while
        // other parameters still refer to it, and every one of them would fail in between.
        //
        // References to the OLD name are not rewritten -- they break, visibly, and say which name
        // they wanted. Silently rewriting text the user wrote elsewhere is the more dangerous of
        // the two, and Fusion refuses renames for the same reason.
        const std::string after = table_->item(row, kName)->text().toStdString();
        if (after == before.name) return;
        const std::string text = before.expression.empty() ? before.value : before.expression;
        ok = controller_.setParameter(after, text);
        if (ok) controller_.removeParameter(before.name);
    }

    if (ok) emit documentChanged();
    // Always: on success to show the new value, on failure to put back what was there. A rejected
    // edit that leaves the typed text sitting in the cell reads as accepted.
    refresh();
}

}  // namespace cadqt
