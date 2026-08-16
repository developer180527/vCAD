#include "PluginManager.h"

#include "Icons.h"

#include "cad/abi/PluginCatalog.h"

#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QUrl>
#include <QVBoxLayout>

namespace cadqt {
namespace {

/// The colour a status is shown in. Only two states are worth colouring: everything is fine, or
/// this one needs the user to do something. Colouring all four turns the column into decoration.
QColor statusColour(cad::abi::PluginStatus status) {
    return status == cad::abi::PluginStatus::Ready ? QColor(0x1f, 0x8a, 0x4c)
                                                   : QColor(0xb3, 0x4a, 0x2f);
}

}  // namespace

PluginManager::PluginManager(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Plugins"));
    setModal(false);
    resize(780, 420);

    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(16, 16, 16, 12);
    column->setSpacing(10);

    // The trust statement, first and unmissable.
    //
    // PLUGIN_CONTRACT.md §4.4 requires this: capabilities are ADVISORY until the sandbox exists,
    // so a native plugin runs with the whole application's privileges whatever its manifest says.
    // Showing a capability list without saying so would present an unenforced declaration as a
    // permission grant, which is worse than showing nothing -- it invites a trust decision the
    // software cannot honour.
    trust_ = new QLabel(
        tr("Plugins run with the same access as vCAD itself. The capabilities listed below are "
           "what each plugin says it needs — they are not enforced. Install plugins only from "
           "sources you trust."),
        this);
    trust_->setWordWrap(true);
    trust_->setObjectName(QStringLiteral("pluginTrustNotice"));
    column->addWidget(trust_);

    table_ = new QTableWidget(this);
    table_->setColumnCount(5);
    table_->setHorizontalHeaderLabels(
        {tr("Plugin"), tr("Version"), tr("Identifier"), tr("Requests"), tr("Status")});
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    table_->verticalHeader()->setVisible(false);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    column->addWidget(table_, 1);

    auto* footer = new QHBoxLayout;
    location_ = new QLabel(this);
    location_->setObjectName(QStringLiteral("pluginLocation"));
    location_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    footer->addWidget(location_, 1);

    auto* open = new QPushButton(tr("Open Folder"), this);
    open->setToolTip(tr("Show the plugin folder in your file manager"));
    connect(open, &QPushButton::clicked, this, [this] {
        const auto dir = cad::abi::userPluginDirectory();
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);   // a folder you can be shown must exist
        QDesktopServices::openUrl(
            QUrl::fromLocalFile(QString::fromStdString(dir.string())));
    });
    footer->addWidget(open);

    auto* rescan = new QPushButton(tr("Rescan"), this);
    // Rescan rather than a file watcher: installing a plugin needs a restart to take effect
    // (§2 -- plugins are not hot-reloadable), so watching the folder would animate a list whose
    // contents cannot be acted on until relaunch. A button is honest about that.
    rescan->setToolTip(tr("Look again for installed plugins"));
    connect(rescan, &QPushButton::clicked, this, [this] { refresh(); });
    footer->addWidget(rescan);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    footer->addWidget(buttons);

    column->addLayout(footer);
    refresh();
}

void PluginManager::refresh() {
    const auto directory = cad::abi::userPluginDirectory();
    const auto plugins = cad::abi::scanInstalledPlugins(directory);

    location_->setText(tr("Plugin folder: %1").arg(QString::fromStdString(directory.string())));

    table_->setRowCount(static_cast<int>(plugins.size()));
    int row = 0;
    for (const auto& plugin : plugins) {
        const auto set = [this, row](int col, const QString& text) {
            auto* item = new QTableWidgetItem(text);
            table_->setItem(row, col, item);
            return item;
        };
        set(0, QString::fromStdString(plugin.displayName));
        set(1, QString::fromStdString(plugin.semver));
        set(2, QString::fromStdString(plugin.id));

        // "none" rather than an empty cell. An absence of text reads as missing information; a
        // plugin that asks for nothing is telling the user something worth seeing.
        const std::string caps = cad::abi::describeCapabilities(plugin.requiredCaps);
        set(3, caps.empty() ? tr("none") : QString::fromStdString(caps));

        auto* status = set(4, QString::fromUtf8(cad::abi::toString(plugin.status)));
        status->setForeground(statusColour(plugin.status));
        if (!plugin.statusDetail.empty()) {
            // The reason lives in the tooltip rather than a column: it is a sentence, and a
            // sentence in a table cell either truncates or ruins the column widths.
            status->setToolTip(QString::fromStdString(plugin.statusDetail));
        }
        ++row;
    }

    if (plugins.empty()) {
        // An empty table with headers looks like a failure to load the list. Say what is true.
        table_->setRowCount(1);
        auto* item = new QTableWidgetItem(tr("No plugins installed."));
        item->setForeground(QColor(0x83, 0x87, 0x8c));
        table_->setItem(0, 0, item);
        table_->setSpan(0, 0, 1, 5);
    }
}

}  // namespace cadqt
