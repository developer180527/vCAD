#include "proshell/SettingsWindow.h"

#include "proshell/Icons.h"
#include "proshell/Settings.h"
#include "proshell/SettingsModel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace proshell {

SettingsWindow::SettingsWindow(Settings& settings, QWidget* parent)
    : QDialog(parent), settings_(settings) {
    setWindowTitle(tr("Settings"));
    resize(760, 520);

    list_ = new QListWidget(this);
    list_->setFixedWidth(190);
    list_->setFrameShape(QFrame::NoFrame);

    pages_ = new QStackedWidget(this);

    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(0);
    row->addWidget(list_);
    row->addWidget(pages_, 1);

    for (const SettingsPage& page : settings_.pages()) buildPage(page);

    connect(list_, &QListWidget::currentRowChanged, pages_, &QStackedWidget::setCurrentIndex);
    if (list_->count() > 0) list_->setCurrentRow(0);
}

void SettingsWindow::showPage(const QString& pageId) {
    const auto at = indexOfPage_.constFind(pageId);
    if (at != indexOfPage_.constEnd()) list_->setCurrentRow(*at);
}

void SettingsWindow::buildPage(const SettingsPage& page) {
    auto* item = new QListWidgetItem(page.label, list_);
    if (!page.iconName.isEmpty()) item->setIcon(icon(page.iconName, 18));

    auto* body = new QWidget(pages_);
    auto* column = new QVBoxLayout(body);
    column->setContentsMargins(20, 18, 20, 18);
    column->setSpacing(16);

    for (const SettingsGroup& group : page.groups) {
        if (!group.label.isEmpty()) {
            auto* heading = new QLabel(group.label.toUpper(), body);
            QFont font = heading->font();
            font.setBold(true);
            font.setPointSizeF(font.pointSizeF() - 1.0);
            heading->setFont(font);
            column->addWidget(heading);
        }

        auto* form = new QFormLayout();
        form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
        form->setSpacing(8);

        for (const Setting& setting : group.settings) {
            const QString id = setting.id;
            QWidget* editor = nullptr;

            switch (setting.kind) {
                case SettingKind::Bool: {
                    auto* box = new QCheckBox(body);
                    box->setChecked(settings_.boolean(id));
                    connect(box, &QCheckBox::toggled, this,
                            [this, id](bool on) { settings_.setValue(id, on); });
                    editor = box;
                    break;
                }
                case SettingKind::Int: {
                    auto* spin = new QSpinBox(body);
                    spin->setRange(static_cast<int>(setting.minimum),
                                   static_cast<int>(setting.maximum));
                    spin->setValue(settings_.integer(id));
                    connect(spin, &QSpinBox::valueChanged, this,
                            [this, id](int v) { settings_.setValue(id, v); });
                    editor = spin;
                    break;
                }
                case SettingKind::Double: {
                    auto* spin = new QDoubleSpinBox(body);
                    spin->setRange(setting.minimum, setting.maximum);
                    spin->setDecimals(3);
                    spin->setValue(settings_.real(id));
                    connect(spin, &QDoubleSpinBox::valueChanged, this,
                            [this, id](double v) { settings_.setValue(id, v); });
                    editor = spin;
                    break;
                }
                case SettingKind::Text: {
                    auto* edit = new QLineEdit(settings_.text(id), body);
                    // editingFinished, not textChanged: a keystroke is not a decision, and writing
                    // one per character would emit a change per letter typed.
                    connect(edit, &QLineEdit::editingFinished, this,
                            [this, id, edit] { settings_.setValue(id, edit->text()); });
                    editor = edit;
                    break;
                }
                case SettingKind::Choice: {
                    auto* combo = new QComboBox(body);
                    combo->addItems(setting.choices);
                    combo->setCurrentIndex(settings_.integer(id));
                    connect(combo, &QComboBox::currentIndexChanged, this,
                            [this, id](int i) { settings_.setValue(id, i); });
                    editor = combo;
                    break;
                }
            }

            if (editor == nullptr) continue;

            // The description sits UNDER the field rather than in a tooltip. A setting whose effect
            // is only discoverable by hovering, or by trying it, is one most users leave alone.
            auto* cell = new QWidget(body);
            auto* cellColumn = new QVBoxLayout(cell);
            cellColumn->setContentsMargins(0, 0, 0, 0);
            cellColumn->setSpacing(2);
            cellColumn->addWidget(editor);
            if (!setting.description.isEmpty()) {
                auto* note = new QLabel(setting.description, cell);
                note->setWordWrap(true);
                note->setEnabled(false);
                cellColumn->addWidget(note);
            }
            form->addRow(setting.label, cell);
        }
        column->addLayout(form);
    }

    column->addStretch(1);

    // Per page, because per field would put a button beside every row for something used rarely.
    auto* resetRow = new QHBoxLayout();
    resetRow->addStretch(1);
    auto* reset = new QPushButton(tr("Reset this page"), body);
    const QString pageId = page.id;
    connect(reset, &QPushButton::clicked, this, [this, pageId] {
        settings_.resetPage(pageId);
        // The widgets still show the old values, and reconciling each one against the store is more
        // machinery than this is worth. Closing is honest about that: reopening shows the truth.
        accept();
    });
    resetRow->addWidget(reset);
    column->addLayout(resetRow);

    auto* scroll = new QScrollArea(pages_);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(body);

    indexOfPage_.insert(page.id, pages_->addWidget(scroll));
}

}  // namespace proshell
