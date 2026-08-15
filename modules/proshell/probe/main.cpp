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
#include <QMainWindow>
#include <QStatusBar>
#include <QVBoxLayout>

#include "proshell/Icons.h"
#include "proshell/MarkingMenu.h"
#include "proshell/Ribbon.h"
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

/// The window, assembled from proshell parts alone.
QMainWindow* buildWindow() {
    auto* window = new QMainWindow;
    window->setWindowTitle(QStringLiteral("proshell probe"));

    auto* ribbon = new proshell::Ribbon(window);
    auto* design = ribbon->addTab(QStringLiteral("Design"));

    auto* build = design->addPanel(QStringLiteral("Build"));
    auto* wall = new QAction(proshell::icon(QStringLiteral("wall")), QStringLiteral("Wall"), window);
    auto* storey =
        new QAction(proshell::icon(QStringLiteral("storey")), QStringLiteral("Storey"), window);
    build->addLarge(wall);
    build->addLarge(storey);

    // The library's own glyphs, which every application shares.
    auto* file = design->addPanel(QStringLiteral("File"));
    for (const auto& name : {"new", "open", "save", "undo", "redo"}) {
        auto* action = new QAction(proshell::icon(QString::fromLatin1(name)),
                                   QString::fromLatin1(name), window);
        file->addSmall(action);
    }

    // A name no provider claims. proshell draws its placeholder rather than an empty icon, which
    // is what makes a missing glyph visible instead of looking like a design choice.
    auto* missing = design->addPanel(QStringLiteral("Unclaimed"));
    missing->addLarge(new QAction(proshell::icon(QStringLiteral("no-such-glyph")),
                                  QStringLiteral("Missing"), window));

    auto* central = new QWidget(window);
    auto* column = new QVBoxLayout(central);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(0);
    column->addWidget(ribbon);

    auto* body = new QLabel(
        QStringLiteral("proshell probe — ribbon, theme, icons and marking menu,\n"
                       "linked without any domain library.\n\nRight-click for the marking menu."),
        central);
    body->setAlignment(Qt::AlignCenter);
    column->addWidget(body, 1);

    body->setContextMenuPolicy(Qt::CustomContextMenu);
    QObject::connect(body, &QWidget::customContextMenuRequested, body, [body](const QPoint& at) {
        std::vector<proshell::MarkingMenu::Item> items;
        items.push_back({QStringLiteral("Wall"), proshell::icon(QStringLiteral("wall"), 20),
                         [] {}, true});
        items.push_back({QStringLiteral("Storey"), proshell::icon(QStringLiteral("storey"), 20),
                         [] {}, true});
        proshell::MarkingMenu::popup(body, body->mapToGlobal(at), std::move(items));
    });

    window->setCentralWidget(central);
    window->statusBar()->showMessage(QStringLiteral("Ready"));
    window->resize(1100, 700);
    return window;
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    proshell::applyTheme(app);
    proshell::IconSet::instance().addProvider(paintProbeGlyph);

    QMainWindow* window = buildWindow();

    // `--check` builds the whole window and exits without an event loop, so CI can run this on a
    // machine with no display and still catch a link error or a construction-time crash. Showing
    // the window is for a human looking at it.
    for (int i = 1; i < argc; ++i) {
        if (QString::fromLocal8Bit(argv[i]) == QLatin1String("--check")) {
            delete window;
            return 0;
        }
    }

    window->show();
    return app.exec();
}
