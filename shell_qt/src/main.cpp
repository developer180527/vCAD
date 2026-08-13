#include "MainWindow.h"
#include "Theme.h"

#include <QApplication>
#include <QPixmap>
#include <QTimer>

#include <cstdio>

namespace {

/// Renders the window to a PNG and exits: `vcad --shot out.png [--tab N]`.
///
/// Here for the same reason the renderer now needs pixel assertions. A UI that is only ever
/// described is a UI nobody has checked, and "the ribbon has an Inspect tab" is a claim about
/// pixels exactly like "the assembly draws 100k parts" is. This makes the claim checkable
/// without a human at the keyboard.
///
/// Two event-loop turns before the grab, not one: the first delivers show and resize, and the
/// layout that follows only lands on the second. Grabbing after one turn catches the window
/// mid-layout, with panels at their pre-layout sizes.
int screenshot(QApplication& app, cadqt::MainWindow& window, const QString& path, int tab,
               bool home) {
    // A populated document, because an empty Home page says nothing about the ribbon and the
    // docks, which is the part being reviewed. `--home` skips it to shoot Home itself.
    if (!home) window.openDemoDocument();
    window.show();
    if (tab >= 0) window.selectRibbonTab(tab);

    QTimer::singleShot(0, &app, [&app, &window, path] {
        QTimer::singleShot(0, &app, [&app, &window, path] {
            const QPixmap shot = window.grab();
            if (!shot.save(path)) {
                std::fprintf(stderr, "could not write %s\n", qPrintable(path));
                app.exit(1);
                return;
            }
            std::printf("wrote %s (%dx%d)\n", qPrintable(path), shot.width(), shot.height());
            app.quit();
        });
    });
    return app.exec();
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("vCAD"));
    app.setOrganizationName(QStringLiteral("vCAD"));

    QString shotPath;
    int shotTab = -1;
    bool shotHome = false;
    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromUtf8(argv[i]);
        if (arg == QStringLiteral("--shot") && i + 1 < argc) {
            shotPath = QString::fromUtf8(argv[++i]);
        } else if (arg == QStringLiteral("--tab") && i + 1 < argc) {
            shotTab = QString::fromUtf8(argv[++i]).toInt();
        } else if (arg == QStringLiteral("--home")) {
            shotHome = true;
        }
    }

    cadqt::applyTheme(app);

    cadqt::MainWindow window;
    if (!shotPath.isEmpty()) return screenshot(app, window, shotPath, shotTab, shotHome);

    window.show();
    return app.exec();
}
