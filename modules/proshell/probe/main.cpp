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
#include <cstdio>
#include <QTableWidget>
#include <QTreeWidget>
#include <QStatusBar>
#include <QVBoxLayout>

#include "proshell/Icons.h"
#include "proshell/MarkingMenu.h"
#include "proshell/Ribbon.h"
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
        auto* rail = new QLabel(QStringLiteral("\n  Home rail\n\n  (the application's,\n"
                                               "  not the frame's)"),
                                this);
        rail->setAlignment(Qt::AlignTop | Qt::AlignLeft);
        rail->setMinimumWidth(180);
        setSidebar(rail, 240);

        home_ = newPage(QStringLiteral("Home — right-click for the marking menu"));
        workspaces()->addWidget(home_);
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

    QLabel* home_ = nullptr;
    int storeys_ = 0;
};

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
            delete window;
            return 0;
        }
        if (arg == QLatin1String("--shot") && i + 1 < argc) {
            shot = QString::fromLocal8Bit(argv[++i]);
        }
    }

    if (!shot.isEmpty()) {
        window->show();
        // Two passes of the event loop: the first delivers show and the resulting layout requests,
        // the second lets the layout settle before the pixels are read. Grabbing after one leaves
        // widgets at their pre-layout geometry, which is a picture of a bug that is not there.
        QApplication::processEvents();
        QApplication::processEvents();
        const bool ok = window->grab().save(shot);
        std::fprintf(stderr, "%s %s\n", ok ? "wrote" : "FAILED to write", qPrintable(shot));
        delete window;
        return ok ? 0 : 1;
    }

    window->show();
    return app.exec();
}
