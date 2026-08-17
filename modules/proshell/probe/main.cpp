// A second application built on proshell, whose only job is to prove the boundary exists.
//
// # Why a whole executable for this
//
// "This library is reusable" is a claim, and a claim with one consumer is one nobody has tested.
// The failure it guards against is specific and quiet: a header in proshell grows an
// `#include "cad/..."`, everything still builds because vcad links the CAD libraries anyway, and
// the coupling is only discovered by the person starting the second application months later.
//
// This binary links proshell and Qt and NOTHING else. If a domain type ever reaches the library,
// this fails to compile or fails to link, in CI, on the commit that introduced it.
//
// It is deliberately not a demo, a template, or a starting point for another application. It is a
// test that happens to have a `main`, and it should stay small enough that nobody is tempted to
// grow features in it.

#include <QApplication>
#include <QLabel>
#include <QPainter>
#include <QMenu>
#include <QTimer>
#include <cstdio>
#include <QTableWidget>
#include <QTreeWidget>
#include <QStatusBar>
#include <QVBoxLayout>

#include "proshell/HomeModel.h"
#include "proshell/HomePage.h"
#include "proshell/Icons.h"
#include "proshell/MarkingMenu.h"
#include "proshell/Ribbon.h"
#include "proshell/Settings.h"
#include "proshell/SettingsModel.h"
#include "proshell/SettingsWindow.h"
#include "proshell/ShellWindow.h"
#include "proshell/Theme.h"

namespace {

/// A glyph vocabulary that is not CAD's, which is the point.
///
/// If proshell had shipped `extrude`, this function would have nothing to demonstrate — an
/// application in a different domain would either use CAD icons or work around the library. Here
/// the library supplies `new`/`open`/`save`/`undo`/`redo` and the application supplies its own
/// nouns, exactly as vcad does.
bool paintProbeGlyph(QPainter& g, const QString& name, int size) {
    if (name == "wall") {
        for (int row = 0; row < 3; ++row) {
            const qreal y = size * (0.30 + row * 0.16);
            g.drawLine(QPointF(size * 0.18, y), QPointF(size * 0.82, y));
        }
        return true;
    }
    if (name == "storey") {
        g.drawRect(QRectF(size * 0.22, size * 0.24, size * 0.56, size * 0.24));
        g.drawRect(QRectF(size * 0.22, size * 0.52, size * 0.56, size * 0.24));
        return true;
    }
    return false;
}

/// The four things the home page asks an application. All of them, for a whole screen.
///
/// Note what is NOT here: the card grid, the search box, the sort control, the list/grid toggle,
/// the empty state, the rail and its links. Those are proshell's, and they are the bulk of the
/// four hundred lines the home page actually is.
class ProbeHomeModel : public proshell::HomeModel {
public:
    [[nodiscard]] QString productName() const override { return QStringLiteral("Probe 0.1"); }
    [[nodiscard]] QString productDetail() const override {
        return QStringLiteral("architecture · not CAD");
    }

    [[nodiscard]] std::vector<proshell::DocumentKind> documentKinds() const override {
        return {{0, QStringLiteral("Storey"), QStringLiteral("storey"), true},
                {1, QStringLiteral("Site"), QStringLiteral("wall"), true},
                {2, QStringLiteral("Schedule"), QStringLiteral("no-such-glyph"), false}};
    }

    [[nodiscard]] std::vector<proshell::RecentDocument> recent() const override { return {}; }

    [[nodiscard]] std::vector<proshell::SummaryField> summary() const override {
        return {{QStringLiteral("Site: none"), false, false},
                {QStringLiteral("● Survey loaded"), true, true}};
    }
};

/// An application on the frame, in a domain that is not CAD.
///
/// Everything below is what a second application would actually write: its own glyphs, its own
/// commands, its own idea of what a "document" is. What it does NOT write is the quick access
/// strip, the File tab, the ribbon, the splitter, the tab bar, the docks or the status bar.
///
/// Note especially that the tab bar is wired here rather than inherited. ShellWindow hands over
/// the widget and stays out of what a tab means — see the note in ShellWindow.h for why a document
/// model shaped by vCAD alone would be worse than none.
class ProbeWindow : public proshell::ShellWindow {
public:
    ProbeWindow() {
        setWindowTitle(QStringLiteral("proshell probe"));
        setProductName(QStringLiteral("Probe"));
        resize(1200, 780);

        buildChrome();
        buildCommands();
        buildPanels();
        buildPages();
    }

private:
    void buildCommands() {
        addQuickAccessButton(QStringLiteral("new"), QStringLiteral("New"),
                             [this] { addStorey(); }, QKeySequence::New);
        addQuickAccessButton(QStringLiteral("open"), QStringLiteral("Open"), [] {});
        addQuickAccessButton(QStringLiteral("save"), QStringLiteral("Save"), [] {});
        addQuickAccessSpacing();
        addQuickAccessButton(QStringLiteral("undo"), QStringLiteral("Undo"), [] {},
                             QKeySequence::Undo);
        addQuickAccessButton(QStringLiteral("redo"), QStringLiteral("Redo"), [] {},
                             QKeySequence::Redo);

        fileMenu()->addAction(QStringLiteral("New Storey"), this, [this] { addStorey(); });
        fileMenu()->addSeparator();
        fileMenu()->addAction(QStringLiteral("Exit"), this, &QWidget::close);

        auto* design = ribbon()->addTab(QStringLiteral("Design"));
        auto* build = design->addPanel(QStringLiteral("Build"));
        build->addLarge(new QAction(proshell::icon(QStringLiteral("wall")),
                                    QStringLiteral("Wall"), this));
        build->addLarge(new QAction(proshell::icon(QStringLiteral("storey")),
                                    QStringLiteral("Storey"), this));

        // A name no provider claims. proshell draws its placeholder rather than an empty icon,
        // which is what makes a missing glyph visible instead of looking like a design choice.
        auto* unclaimed = design->addPanel(QStringLiteral("Unclaimed"));
        unclaimed->addLarge(new QAction(proshell::icon(QStringLiteral("no-such-glyph")),
                                        QStringLiteral("Missing"), this));

        ribbon()->addTab(QStringLiteral("View"));
    }

    void buildPanels() {
        leftDock()->setWindowTitle(QStringLiteral("Model"));
        auto* tree = new QTreeWidget(leftDock());
        tree->setHeaderHidden(true);
        for (const auto* name : {"Site", "Ground Floor", "First Floor"}) {
            tree->addTopLevelItem(new QTreeWidgetItem(QStringList(QString::fromLatin1(name))));
        }
        leftStack()->addWidget(tree);

        auto* properties = new QTableWidget(0, 2, rightDock());
        properties->setHorizontalHeaderLabels({QStringLiteral("Property"), QStringLiteral("Value")});
        rightDock()->setWidget(properties);

        auto* units = new QLabel(QStringLiteral("m"), this);
        addStatusField(units);
        setStatusMessage(QStringLiteral("Ready"));
    }

    void buildPages() {
        // The real home page, on the probe's own model. If a CAD type ever leaks into it, this
        // stops compiling.
        home_ = new proshell::HomePage(homeModel_, workspaces());
        workspaces()->addWidget(home_);
        setSidebar(home_->sidebar(), proshell::HomePage::sidebarDefaultWidth());
        home_->refresh();
        documentTabs()->addTab(QStringLiteral("Home"));

        connect(documentTabs(), &QTabBar::currentChanged, this, [this](int index) {
            if (index < 0) return;
            workspaces()->setCurrentIndex(index);
            setSidebarVisible(index == 0);
            leftDock()->setVisible(index != 0);
            rightDock()->setVisible(index != 0);
        });
        documentTabs()->setCurrentIndex(0);
        setSidebarVisible(true);
        leftDock()->setVisible(false);
        rightDock()->setVisible(false);
    }

    /// The frame has no idea what this means, which is the point.
    void addStorey() {
        const QString name = QStringLiteral("Storey %1").arg(++storeys_);
        workspaces()->addWidget(newPage(name));
        documentTabs()->addTab(name);
        documentTabs()->setCurrentIndex(documentTabs()->count() - 1);
    }

    QLabel* newPage(const QString& text) {
        auto* page = new QLabel(text, this);
        page->setAlignment(Qt::AlignCenter);
        page->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(page, &QWidget::customContextMenuRequested, page, [page](const QPoint& at) {
            std::vector<proshell::MarkingMenu::Item> items;
            items.push_back({QStringLiteral("Wall"), proshell::icon(QStringLiteral("wall"), 20),
                             [] {}, true});
            items.push_back({QStringLiteral("Storey"), proshell::icon(QStringLiteral("storey"), 20),
                             [] {}, true});
            proshell::MarkingMenu::popup(page, page->mapToGlobal(at), std::move(items));
        });
        return page;
    }

    ProbeHomeModel homeModel_;
    proshell::HomePage* home_ = nullptr;
    int storeys_ = 0;
};

/// Paper White is FROZEN, and this is what makes that a fact rather than a comment.
///
/// The reference theme's stylesheet IS the template, so `recolour` returns it untouched and its
/// output is byte-identical by construction. That property is easy to break — one eager
/// substitution, one "tidy up" of the transform — and a theme that has drifted is not something
/// anyone notices from a diff. So: assert the literals survive verbatim.
///
/// It also checks the other three actually CHANGED. A transform with a bug that returns its input
/// would leave all four themes identical and every test about Paper White would still pass.
int checkThemes(QApplication& app) {
    int failures = 0;
    const auto fail = [&failures](const char* what) {
        std::fprintf(stderr, "themes: FAIL %s\n", what);
        ++failures;
    };

    proshell::applyTheme(app, proshell::Theme::PaperWhite);
    const QString reference = app.styleSheet();
    const QColor referenceWindow = app.palette().color(QPalette::Window);

    // The three colours this theme is most identifiable by: warm-grey chrome, its hairline, and the
    // Inventor blue. If these are intact the transform did not touch the reference.
    if (!reference.contains(QLatin1String("#f0efed"))) fail("Paper White lost its chrome colour");
    if (!reference.contains(QLatin1String("#cfcdc9"))) fail("Paper White lost its hairline colour");
    if (!reference.contains(QLatin1String("#0a6cc4"))) fail("Paper White lost its accent colour");
    if (referenceWindow != QColor(0xf0, 0xef, 0xed)) fail("Paper White's window colour moved");

    if (proshell::themeNames().size() != 4) fail("themeNames does not list four themes");

    for (const auto theme : {proshell::Theme::ClassicWhite, proshell::Theme::Midnight,
                             proshell::Theme::ClassicDark}) {
        proshell::applyTheme(app, theme);
        if (app.styleSheet() == reference) fail("a theme produced the reference unchanged");
        if (app.styleSheet().isEmpty()) fail("a theme produced an empty stylesheet");
        // Every literal must have been rewritten. A leftover light colour on a dark theme is the
        // failure this catches, and it is invisible until someone looks at that exact widget.
        if (app.styleSheet().contains(QLatin1String("#f0efed"))) {
            fail("a theme left a Paper White literal behind");
        }
    }

    // The dark themes must actually be dark, and differ from each other only in hue -- which is the
    // entire reason for offering both Midnight and Classic Dark.
    proshell::applyTheme(app, proshell::Theme::Midnight);
    const QColor midnight = app.palette().color(QPalette::Window);
    proshell::applyTheme(app, proshell::Theme::ClassicDark);
    const QColor classicDark = app.palette().color(QPalette::Window);

    if (midnight.lightness() > 110) fail("Midnight is not dark");
    if (classicDark.lightness() > 110) fail("Classic Dark is not dark");
    if (classicDark.saturation() > 12) fail("Classic Dark is not neutral");
    if (midnight.saturation() < 12) fail("Midnight has no blue in it");

    // Leave the application on the reference, so a --shot after this is the theme people expect.
    proshell::applyTheme(app, proshell::Theme::PaperWhite);

    if (failures == 0) std::printf("themes: OK\n");
    return failures;
}

/// The settings store's contract, checked from the one binary that links no domain code.
///
/// Here rather than in the CAD test suite for the same reason the probe exists at all: settings are
/// meant to serve a SECOND application, and a test that runs inside the first one proves nothing
/// about that. Returns the number of failures.
int checkSettings() {
    int failures = 0;
    const auto fail = [&failures](const char* what) {
        std::fprintf(stderr, "settings: FAIL %s\n", what);
        ++failures;
    };

    // A scope of its own, cleared first, so a developer's real preferences are never read or
    // written by a test run.
    proshell::Settings settings(QStringLiteral("proshell-probe"), QStringLiteral("settings-check"));

    proshell::SettingsPage page;
    page.id = QStringLiteral("probe.general");
    page.label = QStringLiteral("General");
    proshell::SettingsGroup group;
    group.label = QStringLiteral("Behaviour");

    proshell::Setting flag;
    flag.id = QStringLiteral("probe.flag");
    flag.label = QStringLiteral("A flag");
    flag.kind = proshell::SettingKind::Bool;
    flag.fallback = false;
    group.settings.push_back(flag);

    proshell::Setting count;
    count.id = QStringLiteral("probe.count");
    count.label = QStringLiteral("A count");
    count.kind = proshell::SettingKind::Int;
    count.fallback = 7;
    count.minimum = 0;
    count.maximum = 100;
    group.settings.push_back(count);

    page.groups.push_back(group);
    settings.addPage(page);

    settings.resetPage(page.id);   // start from declared defaults whatever a previous run left

    // The fallback is what an untouched setting reads as.
    if (settings.integer(QStringLiteral("probe.count")) != 7) fail("fallback not returned");
    if (settings.isOverridden(QStringLiteral("probe.count"))) fail("untouched reads as overridden");

    // An UNKNOWN id must not answer. Returning a default-constructed value would let a typo look
    // like a real `false` and be acted on.
    if (settings.value(QStringLiteral("probe.nope")).isValid()) fail("unknown id answered");
    if (settings.boolean(QStringLiteral("probe.nope"), true) != true) {
        fail("unknown id ignored the caller's own fallback");
    }

    // Storing something nothing declared is refused, so a stale id in a saved layout cannot
    // resurrect a setting that no longer exists.
    settings.setValue(QStringLiteral("probe.nope"), 1);
    if (settings.value(QStringLiteral("probe.nope")).isValid()) fail("undeclared id was stored");

    int changes = 0;
    QObject::connect(&settings, &proshell::Settings::changed,
                     [&changes](const QString&, const QVariant&) { ++changes; });

    settings.setValue(QStringLiteral("probe.count"), 12);
    if (settings.integer(QStringLiteral("probe.count")) != 12) fail("value did not persist");
    if (!settings.isOverridden(QStringLiteral("probe.count"))) fail("override not recorded");
    if (changes != 1) fail("a change did not notify exactly once");

    // Writing the same value again must be silent. A dialog that writes every field on close would
    // otherwise emit a storm of changes nothing acted on.
    settings.setValue(QStringLiteral("probe.count"), 12);
    if (changes != 1) fail("an unchanged write notified");

    // Reset REMOVES the key rather than writing the default. The difference shows up on the next
    // release: a value stored explicitly survives a change to the default, and a user who never
    // overrode it should follow the new one.
    settings.reset(QStringLiteral("probe.count"));
    if (settings.isOverridden(QStringLiteral("probe.count"))) fail("reset left the key behind");
    if (settings.integer(QStringLiteral("probe.count")) != 7) fail("reset did not restore fallback");

    // Merging into an existing page is the normal case; a duplicate SETTING id inside it is not.
    proshell::SettingsPage more;
    more.id = QStringLiteral("probe.general");
    proshell::SettingsGroup extra;
    extra.label = QStringLiteral("More");
    extra.settings.push_back(flag);   // a duplicate id, which must be dropped
    proshell::Setting fresh;
    fresh.id = QStringLiteral("probe.fresh");
    fresh.label = QStringLiteral("Fresh");
    fresh.kind = proshell::SettingKind::Bool;
    fresh.fallback = true;
    extra.settings.push_back(fresh);
    more.groups.push_back(extra);
    settings.addPage(more);

    if (settings.pages().size() != 1) fail("merging created a second page");
    if (!settings.boolean(QStringLiteral("probe.fresh"))) fail("merged setting not readable");

    std::size_t flagCount = 0;
    for (const auto& p : settings.pages()) {
        for (const auto& g : p.groups) {
            for (const auto& s : g.settings) {
                if (s.id == QLatin1String("probe.flag")) ++flagCount;
            }
        }
    }
    if (flagCount != 1) fail("a duplicate setting id was accepted");

    // And the window must construct over whatever the store holds. A settings system whose window
    // crashes on a Choice with no choices is not reusable, it is a trap.
    proshell::SettingsWindow window(settings);
    window.showPage(QStringLiteral("probe.general"));

    if (failures == 0) std::printf("settings: OK\n");
    return failures;
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    proshell::applyTheme(app);
    proshell::IconSet::instance().addProvider(paintProbeGlyph);

    auto* window = new ProbeWindow;

    // `--check` builds the whole window and exits without an event loop, so CI can run this on a
    // machine with no display and still catch a link error or a construction-time crash.
    //
    // `--shot <path>` renders it instead. Worth the dozen lines: --check would have passed happily
    // while the document tab bar was being laid out at zero height and was simply not there. A
    // frame is a visual thing, and the only check that catches one which constructs correctly and
    // LOOKS wrong is a picture of it.
    QString shot;
    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg == QLatin1String("--check")) {
            const int failures = checkSettings() + checkThemes(app);
            delete window;
            return failures == 0 ? 0 : 1;
        }
        if (arg == QLatin1String("--shot") && i + 1 < argc) {
            shot = QString::fromLocal8Bit(argv[++i]);
        }
    }

    if (!shot.isEmpty()) {
        window->show();

        // Two turns of the REAL event loop, not two calls to processEvents(). The first delivers
        // show and resize, and the layout that follows only lands on the second. processEvents()
        // is not equivalent: deferred deletes and posted layout requests need actual loop turns,
        // and grabbing without them catches the window mid-layout -- widgets stacked at their
        // pre-layout positions, which photographs as a rendering bug that is not there.
        QTimer::singleShot(0, &app, [&app, window, shot] {
            QTimer::singleShot(0, &app, [&app, window, shot] {
                const bool ok = window->grab().save(shot);
                std::fprintf(stderr, "%s %s\n", ok ? "wrote" : "FAILED to write",
                             qPrintable(shot));
                app.exit(ok ? 0 : 1);
            });
        });
        const int code = app.exec();
        delete window;
        return code;
    }

    window->show();
    return app.exec();
}
