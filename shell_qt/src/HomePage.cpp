#include "HomePage.h"

#include "Icons.h"

#include <QFrame>
#include <QGridLayout>
#include <QLabel>
#include <QListWidget>
#include <QToolButton>
#include <QVBoxLayout>

namespace cadqt {
namespace {

/// A big New tile, matching Inventor's New panel. Disabled tiles are still SHOWN: a user should
/// see that Assembly exists and is coming, not wonder whether the app has one.
QToolButton* newTile(QWidget* parent, cad::app::DocumentKind kind) {
    auto* button = new QToolButton(parent);
    const QString label = QString::fromUtf8(cad::app::toString(kind));
    button->setText(cad::app::implemented(kind) ? label
                                               : label + QObject::tr("\n(not yet)"));
    button->setIcon(icon(label.toLower(), 48));
    button->setIconSize(QSize(48, 48));
    button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    button->setFixedSize(112, 100);
    button->setEnabled(cad::app::implemented(kind));
    button->setObjectName(QStringLiteral("homeTile"));
    button->setToolTip(cad::app::implemented(kind)
                           ? QObject::tr("Create a new %1").arg(label)
                           : QObject::tr("%1 documents are not implemented yet").arg(label));
    return button;
}

}  // namespace

HomePage::HomePage(cad::app::Session& session, QWidget* parent)
    : QWidget(parent), session_(session) {
    setObjectName(QStringLiteral("homePage"));
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(32, 28, 32, 28);
    outer->setSpacing(18);

    auto* title = new QLabel(tr("vCAD"), this);
    title->setObjectName(QStringLiteral("homeTitle"));
    outer->addWidget(title);

    auto* newLabel = new QLabel(tr("New"), this);
    newLabel->setObjectName(QStringLiteral("homeSection"));
    outer->addWidget(newLabel);

    auto* tiles = new QWidget(this);
    auto* tileRow = new QGridLayout(tiles);
    tileRow->setContentsMargins(0, 0, 0, 0);
    tileRow->setSpacing(10);
    int column = 0;
    for (const auto kind : {cad::app::DocumentKind::Part, cad::app::DocumentKind::Assembly,
                            cad::app::DocumentKind::Drawing,
                            cad::app::DocumentKind::Presentation}) {
        auto* tile = newTile(tiles, kind);
        const int k = static_cast<int>(kind);
        connect(tile, &QToolButton::clicked, this, [this, k] { emit createRequested(k); });
        tileRow->addWidget(tile, 0, column++);
    }
    tileRow->setColumnStretch(column, 1);
    outer->addWidget(tiles);

    auto* line = new QFrame(this);
    line->setFrameShape(QFrame::HLine);
    line->setObjectName(QStringLiteral("homeRule"));
    outer->addWidget(line);

    auto* recentLabel = new QLabel(tr("Recent documents"), this);
    recentLabel->setObjectName(QStringLiteral("homeSection"));
    outer->addWidget(recentLabel);

    recent_ = new QListWidget(this);
    recent_->setObjectName(QStringLiteral("homeRecent"));
    outer->addWidget(recent_, 1);
    connect(recent_, &QListWidget::itemActivated, this, [this](QListWidgetItem* item) {
        emit openRequested(item->data(Qt::UserRole).toString());
    });

    refresh();
}

void HomePage::refresh() {
    recent_->clear();
    if (session_.recent().empty()) {
        // Say what will appear here rather than leaving a blank box, which reads as broken.
        auto* empty = new QListWidgetItem(tr("Documents you open will be listed here."), recent_);
        empty->setFlags(Qt::NoItemFlags);
        return;
    }
    for (const auto& path : session_.recent()) {
        auto* item = new QListWidgetItem(QString::fromStdString(path.filename().string()), recent_);
        item->setData(Qt::UserRole, QString::fromStdString(path.string()));
        item->setToolTip(QString::fromStdString(path.string()));
    }
}

}  // namespace cadqt
