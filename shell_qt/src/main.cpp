#include "MainWindow.h"
#include "Theme.h"

#include "cad/kernel/Diagnostics.h"
#include "cad/log/Log.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QStandardPaths>

#include <QApplication>
#include <QPixmap>
#include <QTimer>

#include <cstdio>

namespace {

/// Sends Qt's own messages through our log.
///
/// Qt reports real problems this way — a stylesheet it could not parse, a layout it could not
/// satisfy, a missing image — and by default they go to stderr where a GUI user never sees them.
void qtMessageHandler(QtMsgType type, const QMessageLogContext& context, const QString& message) {
    using cad::log::Category;
    const std::string text = message.toStdString();
    const char* file = context.file != nullptr ? context.file : "qt";
    // Constructed by hand rather than through the macros, because the file and line belong to Qt's
    // call site rather than to this function, and the macros would report this line for everything.
    cad::log::Record record;
    record.category = Category::ThirdParty;
    record.file = file;
    record.line = context.line;
    record.message = "Qt: " + text;
    switch (type) {
        case QtDebugMsg:    record.level = cad::log::Level::Debug; break;
        case QtInfoMsg:     record.level = cad::log::Level::Info; break;
        case QtWarningMsg:  record.level = cad::log::Level::Warning; break;
        default:            record.level = cad::log::Level::Error; break;
    }
    if (!cad::log::enabled(record.category, record.level)) return;
    record.timestampUs =
        static_cast<std::int64_t>(QDateTime::currentMSecsSinceEpoch()) * 1000;
    cad::log::write(std::move(record));
}

/// Sets up logging before anything else runs.
///
/// A file sink in the platform's application-data directory, because a log a user cannot find is a
/// log that never reaches a bug report. `QStandardPaths` puts it where each platform expects:
/// ~/Library/Application Support on macOS, %APPDATA% on Windows, ~/.local/share on Linux.
QString startLogging() {
    using cad::log::Category;
    using cad::log::Level;

    // Info by default, Warning for the noisy ones. ThirdParty at Warning specifically: OCCT's
    // healing and import chatter is valuable when you are chasing a bad file and pure noise
    // otherwise, and it is exactly the category a user can be asked to raise.
    cad::log::setLevel(Level::Info);
    cad::log::setLevel(Category::ThirdParty, Level::Warning);
    cad::log::setLevel(Category::Recompute, Level::Info);

    // CAD_LOG_LEVEL=debug raises everything without a rebuild, which is what a support
    // conversation actually needs -- "run it again with this set" beats "install a debug build".
    if (const char* raw = std::getenv("CAD_LOG_LEVEL")) {
        const std::string value(raw);
        if (value == "trace") cad::log::setLevel(Level::Trace);
        else if (value == "debug") cad::log::setLevel(Level::Debug);
        else if (value == "warn") cad::log::setLevel(Level::Warning);
        else if (value == "error") cad::log::setLevel(Level::Error);
        else if (value == "off") cad::log::setLevel(Level::Off);
    }

    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QString path;
    if (!dir.isEmpty() && QDir().mkpath(dir)) {
        path = QDir(dir).filePath(QStringLiteral("vcad.log"));
        cad::log::addSink(cad::log::fileSink(path.toStdString()));
    }

    // OCCT last, so its own startup output lands in a log that already has its sinks.
    cad::kernel::routeOcctDiagnosticsToLog();
    qInstallMessageHandler(qtMessageHandler);
    return path;
}

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
    // The names are STATIC and must be set before QStandardPaths is asked anything: the writable
    // AppDataLocation is derived from them, so setting them after QApplication would put the log in
    // an unnamed directory.
    QCoreApplication::setApplicationName(QStringLiteral("vCAD"));
    QCoreApplication::setOrganizationName(QStringLiteral("vCAD"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.0.1"));

    // Before QApplication, so a failure constructing it is still recorded.
    const QString logPath = startLogging();
    CAD_INFO(cad::log::Category::Shell)
        << "vCAD " << qPrintable(QCoreApplication::applicationVersion()) << " starting"
        << (logPath.isEmpty() ? "" : " — log: ") << qPrintable(logPath);

    QApplication app(argc, argv);

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
