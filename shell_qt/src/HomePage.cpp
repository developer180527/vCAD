#include "HomePage.h"

#include "Icons.h"

#include <QButtonGroup>
#include <QComboBox>
#include <QDateTime>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QScrollArea>
#include <QToolButton>
#include <QVBoxLayout>
#include <QStyle>

namespace cadqt {
namespace {

using cad::app::DocumentKind;

/// Default, and the range the drag handle allows. The default is what Inventor's rail measures;
/// the bounds exist so a drag cannot hide the rail or swallow the page.
constexpr int kSidebarWidth = 250;
constexpr int kSidebarMinWidth = 190;
constexpr int kSidebarMaxWidth = 420;
constexpr int kCardWidth = 168;
constexpr int kThumbHeight = 116;
/// Four across before wrapping. Inventor reflows to the window; a fixed column count is the
/// cheap version of that and is what the default window width gives anyway.
constexpr int kCardColumns = 4;

const std::array<DocumentKind, 4> kKinds{DocumentKind::Part, DocumentKind::Assembly,
                                         DocumentKind::Drawing, DocumentKind::Presentation};

QFrame* horizontalRule(QWidget* parent) {
    auto* line = new QFrame(parent);
    line->setFrameShape(QFrame::HLine);
    line->setObjectName(QStringLiteral("homeRule"));
    return line;
}

QLabel* separatorDot(QWidget* parent) {
    auto* dot = new QLabel(QStringLiteral("│"), parent);
    dot->setObjectName(QStringLiteral("homeStripSeparator"));
    return dot;
}

/// "22/11/2024 16:41", Inventor's format. Relative time reads better in a list you scan daily;
/// an absolute stamp is what an engineer quotes in a change note, which is the more common use.
QString timestampOf(const std::filesystem::path& path) {
    const QFileInfo info(QString::fromStdString(path.string()));
    if (!info.exists()) return QObject::tr("not found");
    return info.lastModified().toString(QStringLiteral("dd/MM/yyyy HH:mm"));
}

}  // namespace

HomePage::HomePage(cad::app::Session& session, QWidget* parent)
    : QWidget(parent), session_(session) {
    setObjectName(QStringLiteral("homePage"));

    // Parentless on purpose: MainWindow takes ownership when it adds this to the window's left
    // column. See sidebar().
    sidebar_ = buildSidebar();

    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(28, 0, 28, 20);
    column->setSpacing(0);

    column->addWidget(buildProjectStrip());
    column->addSpacing(22);

    auto* heading = new QLabel(tr("Recent"), this);
    heading->setObjectName(QStringLiteral("homeHeading"));
    column->addWidget(heading);
    column->addSpacing(10);

    column->addWidget(buildRecentToolbar());
    column->addSpacing(16);

    auto* scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("homeScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->viewport()->setAutoFillBackground(false);

    cardsHost_ = new QWidget(scroll);
    cardsHost_->setObjectName(QStringLiteral("homeCards"));
    cardsHost_->setAutoFillBackground(false);
    cards_ = new QGridLayout(cardsHost_);
    cards_->setContentsMargins(0, 0, 0, 0);
    cards_->setSpacing(14);
    cards_->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    scroll->setWidget(cardsHost_);
    column->addWidget(scroll, 1);

    refresh();
}

int HomePage::sidebarDefaultWidth() noexcept { return kSidebarWidth; }

// ── sidebar ─────────────────────────────────────────────────────────────────────────────

QWidget* HomePage::buildSidebar() {
    // Parented to the page for OWNERSHIP, even though MainWindow lays it out. In Qt a parent is
    // ownership, not layout membership: QLayout::addWidget reparents whatever it is given, so the
    // window can still place this in its left column. A parentless QWidget, by contrast, is a
    // top-level window that nothing will ever delete — it leaks if the window does not adopt it,
    // and flashes as a stray window if it is shown before it does.
    auto* side = new QWidget(this);
    side->setObjectName(QStringLiteral("homeSidebar"));

    // Resizable, not fixed: a drag handle lives between this and the content (see
    // MainWindow::buildWorkspaces). Bounds rather than a fixed width — narrow enough that the
    // longest label, "Open file...", still fits, and wide enough to stop before it dominates.
    side->setMinimumWidth(kSidebarMinWidth);
    side->setMaximumWidth(kSidebarMaxWidth);

    auto* column = new QVBoxLayout(side);
    column->setContentsMargins(26, 30, 26, 22);
    column->setSpacing(0);

    auto* product = new QLabel(tr("vCAD 0.1"), side);
    product->setObjectName(QStringLiteral("homeProduct"));
    column->addWidget(product);
    column->addSpacing(4);

    auto* milestone = new QLabel(tr("M3 · renderer"), side);
    milestone->setObjectName(QStringLiteral("homeProductSub"));
    column->addWidget(milestone);
    column->addSpacing(26);
    column->addWidget(horizontalRule(side));
    column->addSpacing(18);

    auto* open = new QPushButton(tr("Open..."), side);
    open->setObjectName(QStringLiteral("homeSideButton"));
    connect(open, &QPushButton::clicked, this, [this] { emit openBrowseRequested(); });
    column->addWidget(open);
    column->addSpacing(8);

    // New..., with the kinds on a dropdown — Inventor's split button. The kinds that are not
    // implemented are listed and disabled rather than hidden (ADR 0009 decision 2).
    auto* create = new QToolButton(side);
    create->setText(tr("New..."));
    create->setObjectName(QStringLiteral("homeSideButton"));
    create->setPopupMode(QToolButton::MenuButtonPopup);
    create->setToolButtonStyle(Qt::ToolButtonTextOnly);
    auto* menu = new QMenu(create);
    for (const auto kind : kKinds) {
        const QString label = QString::fromUtf8(cad::app::toString(kind));
        auto* action = menu->addAction(icon(label.toLower(), 16), label);
        action->setEnabled(cad::app::implemented(kind));
        const int k = static_cast<int>(kind);
        connect(action, &QAction::triggered, this, [this, k] { emit createRequested(k); });
    }
    create->setMenu(menu);
    connect(create, &QToolButton::clicked, this,
            [this] { emit createRequested(static_cast<int>(DocumentKind::Part)); });
    column->addWidget(create);

    column->addStretch(1);

    column->addWidget(horizontalRule(side));
    column->addSpacing(12);
    for (const auto& [text, enabled] : std::initializer_list<std::pair<QString, bool>>{
             {tr("What's New"), false}, {tr("Help"), false},
             {tr("Tutorials"), false}, {tr("Community"), false}}) {
        auto* link = new QLabel(text, side);
        link->setObjectName(enabled ? QStringLiteral("homeLink")
                                    : QStringLiteral("homeLinkDisabled"));
        column->addWidget(link);
        column->addSpacing(6);
    }
    return side;
}

// ── main area ───────────────────────────────────────────────────────────────────────────

QWidget* HomePage::buildProjectStrip() {
    // Ours, not Inventor's (DESKTOP_UX 3.7). "Open this project" also means "use the team's
    // cache", so the cache belongs next to the project rather than buried in preferences.
    auto* strip = new QWidget(this);
    strip->setObjectName(QStringLiteral("homeProjectStrip"));
    auto* row = new QHBoxLayout(strip);
    row->setContentsMargins(0, 14, 0, 12);
    row->setSpacing(10);

    projectName_ = new QLabel(strip);
    projectName_->setObjectName(QStringLiteral("homeStripItem"));
    row->addWidget(projectName_);
    row->addWidget(separatorDot(strip));

    projectPaths_ = new QLabel(strip);
    projectPaths_->setObjectName(QStringLiteral("homeStripItem"));
    row->addWidget(projectPaths_);
    row->addWidget(separatorDot(strip));

    cacheStatus_ = new QLabel(strip);
    cacheStatus_->setObjectName(QStringLiteral("homeStripCache"));
    row->addWidget(cacheStatus_);
    row->addStretch(1);
    return strip;
}

QWidget* HomePage::buildRecentToolbar() {
    auto* bar = new QWidget(this);
    auto* row = new QHBoxLayout(bar);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(8);

    // List / grid toggle, as a pair of checkable buttons like Inventor's.
    auto* views = new QButtonGroup(this);
    views->setExclusive(true);
    int viewId = 0;
    for (const auto& name : {QStringLiteral("view-list"), QStringLiteral("view-grid")}) {
        auto* button = new QToolButton(bar);
        button->setIcon(icon(name, 16));
        button->setCheckable(true);
        button->setAutoRaise(true);
        button->setObjectName(QStringLiteral("homeViewToggle"));
        button->setToolTip(viewId == 0 ? tr("List view") : tr("Grid view"));
        if (viewId == 1) button->setChecked(true);
        views->addButton(button, viewId++);
        row->addWidget(button);
    }
    connect(views, &QButtonGroup::idClicked, this, [this](int id) {
        gridView_ = id == 1;
        rebuildCards();
    });

    row->addWidget(separatorDot(bar));

    auto* sortLabel = new QLabel(tr("Sort by"), bar);
    sortLabel->setObjectName(QStringLiteral("homeStripItem"));
    row->addWidget(sortLabel);

    sort_ = new QComboBox(bar);
    sort_->addItems({tr("Last Opened"), tr("Name"), tr("Kind")});
    sort_->setObjectName(QStringLiteral("homeSort"));
    connect(sort_, &QComboBox::currentIndexChanged, this, [this](int) { rebuildCards(); });
    row->addWidget(sort_);

    row->addStretch(1);

    search_ = new QLineEdit(bar);
    search_->setPlaceholderText(tr("Search recent"));
    search_->setObjectName(QStringLiteral("homeSearch"));
    search_->setFixedWidth(220);
    search_->setClearButtonEnabled(true);
    connect(search_, &QLineEdit::textChanged, this, [this](const QString&) { rebuildCards(); });
    row->addWidget(search_);

    return bar;
}

// ── cards ───────────────────────────────────────────────────────────────────────────────

/// A card that reports clicks.
///
/// No Q_OBJECT and no signal of its own — it takes a callback — so this needs no moc pass and can
/// live in the .cpp where it belongs. A QPushButton would give clicks for free but not a
/// thumbnail-over-two-labels layout without fighting its own painting.
class ClickableCard : public QFrame {
public:
    ClickableCard(QWidget* parent, std::function<void()> onActivate)
        : QFrame(parent), onActivate_(std::move(onActivate)) {}

protected:
    void mouseReleaseEvent(QMouseEvent* event) override {
        // Only a release that lands inside counts, so dragging off the card cancels — the standard
        // button contract, and users do rely on it to back out of a misclick.
        if (event->button() == Qt::LeftButton && rect().contains(event->pos()) && onActivate_) {
            onActivate_();
        }
        QFrame::mouseReleaseEvent(event);
    }

private:
    std::function<void()> onActivate_;
};

QWidget* HomePage::buildCard(const std::filesystem::path& path) {
    const QString asText = QString::fromStdString(path.string());
    auto* card = new ClickableCard(cardsHost_, [this, asText] { emit openRequested(asText); });
    card->setObjectName(QStringLiteral("homeCard"));
    card->setFixedWidth(kCardWidth);
    card->setCursor(Qt::PointingHandCursor);

    auto* column = new QVBoxLayout(card);
    column->setContentsMargins(8, 8, 8, 10);
    column->setSpacing(6);

    // Thumbnail. A rendered preview belongs here; until documents can be opened and drawn there
    // is nothing to render, so the kind's icon stands in rather than a grey rectangle that looks
    // like a failed image load.
    auto* thumb = new QLabel(card);
    thumb->setObjectName(QStringLiteral("homeThumb"));
    thumb->setFixedHeight(kThumbHeight);
    thumb->setAlignment(Qt::AlignCenter);
    thumb->setPixmap(icon(QStringLiteral("part"), 48).pixmap(48, 48));
    column->addWidget(thumb);

    auto* name = new QLabel(QString::fromStdString(path.filename().string()), card);
    name->setObjectName(QStringLiteral("homeCardName"));
    name->setWordWrap(false);
    column->addWidget(name);

    auto* when = new QLabel(timestampOf(path), card);
    when->setObjectName(QStringLiteral("homeCardWhen"));
    column->addWidget(when);

    card->setToolTip(QString::fromStdString(path.string()));
    return card;
}

QWidget* HomePage::buildEmptyState() {
    auto* empty = new QWidget(cardsHost_);
    auto* column = new QVBoxLayout(empty);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(14);

    auto* text = new QLabel(tr("No recent documents. Start one:"), empty);
    text->setObjectName(QStringLiteral("homeEmpty"));
    column->addWidget(text);

    auto* tiles = new QWidget(empty);
    auto* tileRow = new QHBoxLayout(tiles);
    tileRow->setContentsMargins(0, 0, 0, 0);
    tileRow->setSpacing(10);
    for (const auto kind : kKinds) {
        const QString label = QString::fromUtf8(cad::app::toString(kind));
        const bool ready = cad::app::implemented(kind);
        auto* tile = new QToolButton(tiles);
        // Disabled tiles are still SHOWN. A user should see that Assembly exists and is coming,
        // not wonder whether the app has one.
        tile->setText(ready ? label : label + tr("\n(not yet)"));
        tile->setIcon(icon(label.toLower(), 48));
        tile->setIconSize(QSize(48, 48));
        tile->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        tile->setFixedSize(112, 104);
        tile->setEnabled(ready);
        tile->setObjectName(QStringLiteral("homeTile"));
        tile->setToolTip(ready ? tr("Create a new %1").arg(label)
                               : tr("%1 documents are not implemented yet").arg(label));
        const int k = static_cast<int>(kind);
        connect(tile, &QToolButton::clicked, this, [this, k] { emit createRequested(k); });
        tileRow->addWidget(tile);
    }
    tileRow->addStretch(1);
    column->addWidget(tiles);
    column->addStretch(1);
    return empty;
}

void HomePage::rebuildCards() {
    while (QLayoutItem* item = cards_->takeAt(0)) {
        if (QWidget* w = item->widget()) w->deleteLater();
        delete item;
    }

    const QString needle = search_ != nullptr ? search_->text().trimmed() : QString();
    std::vector<std::filesystem::path> shown;
    for (const auto& path : session_.recent()) {
        const QString name = QString::fromStdString(path.filename().string());
        if (!needle.isEmpty() && !name.contains(needle, Qt::CaseInsensitive)) continue;
        shown.push_back(path);
    }

    if (sort_ != nullptr && sort_->currentIndex() == 1) {
        std::sort(shown.begin(), shown.end(), [](const auto& a, const auto& b) {
            return a.filename().string() < b.filename().string();
        });
    }

    if (shown.empty()) {
        cards_->addWidget(session_.recent().empty()
                              ? buildEmptyState()
                              : [this] {
                                    auto* none = new QLabel(tr("Nothing matches that search."),
                                                            cardsHost_);
                                    none->setObjectName(QStringLiteral("homeEmpty"));
                                    return static_cast<QWidget*>(none);
                                }(),
                          0, 0);
        return;
    }

    const int columns = gridView_ ? kCardColumns : 1;
    int index = 0;
    for (const auto& path : shown) {
        cards_->addWidget(buildCard(path), index / columns, index % columns);
        ++index;
    }
}

// ── refresh ─────────────────────────────────────────────────────────────────────────────

void HomePage::refresh() {
    const auto& project = session_.project();
    projectName_->setText(project.loaded()
                              ? tr("Project: %1").arg(QString::fromStdString(project.name))
                              : tr("Project: none"));
    projectPaths_->setText(tr("Search paths: %1 configured")
                               .arg(project.searchPaths.size()));

    // Honest about the cache: "online" is a claim about a directory we have not checked. Until
    // the DDC reports its own status this says what is configured, not what is reachable.
    if (project.sharedCache.empty()) {
        cacheStatus_->setText(tr("○ Shared cache not configured"));
        cacheStatus_->setProperty("online", false);
    } else {
        cacheStatus_->setText(
            tr("● Shared cache: %1").arg(QString::fromStdString(project.sharedCache.string())));
        cacheStatus_->setProperty("online", true);
    }
    cacheStatus_->style()->unpolish(cacheStatus_);
    cacheStatus_->style()->polish(cacheStatus_);

    rebuildCards();
}

}  // namespace cadqt
