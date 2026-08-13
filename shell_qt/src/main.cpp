#include "MainWindow.h"
#include "Theme.h"

#include "cad/kernel/Diagnostics.h"
#include "cad/log/Log.h"
#include "cad/render/BgfxBackend.h"

#include <QDateTime>
#include <filesystem>
#include <string>

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

/// Sets up logging before anything else runs, and returns where the log went.
///
/// Resolved from argv[0], not QStandardPaths. vCAD ships as a bare executable today -- there is no
/// .app bundle and no installer -- so the platform's application-data directory is the wrong
/// answer twice over: it is for packaged applications, and while developing it puts the log
/// somewhere you have to go hunting for, surviving rebuilds that should have cleaned it up.
///
/// Beside the executable is where a developer looks first and where a `rm -rf build` correctly
/// removes it. Deriving it from argv[0] also means no Qt is involved, so logging is running before
/// anything else in the process -- and it removes the ordering trap that using QStandardPaths
/// introduced, where the directory silently depended on names set later.
///
/// Order of preference, first that works:
///   1. $CAD_LOG_FILE       -- explicit always wins, and is what a support conversation asks for
///   2. beside the executable
///   3. the temp directory  -- an installed binary may live somewhere read-only
std::string resolveLogPath(const char* argv0) {
    namespace fs = std::filesystem;

    if (const char* explicitPath = std::getenv("CAD_LOG_FILE")) {
        if (*explicitPath != '\0') return explicitPath;
    }

    std::error_code ec;
    if (argv0 != nullptr && *argv0 != '\0') {
        fs::path exe = fs::weakly_canonical(fs::path(argv0), ec);
        if (!ec && exe.has_parent_path()) {
            const fs::path candidate = exe.parent_path() / "vcad.log";
            // Probed by opening rather than by checking permissions: the permission bits can say
            // yes on a read-only mount, and the only honest test of "can I write here" is writing.
            if (std::FILE* probe = std::fopen(candidate.string().c_str(), "ab")) {
                std::fclose(probe);
                return candidate.string();
            }
        }
    }

    const fs::path fallback = fs::temp_directory_path(ec) / "vcad.log";
    return ec ? std::string{} : fallback.string();
}

std::string startLogging(const char* argv0) {
    using cad::log::Category;
    using cad::log::Level;

    // Info by default, Warning for the noisy one. OCCT's healing and import chatter is valuable
    // when you are chasing a bad file and pure noise otherwise, and it is exactly the category a
    // user can be asked to raise.
    cad::log::setLevel(Level::Info);
    cad::log::setLevel(Category::ThirdParty, Level::Warning);

    // CAD_LOG_LEVEL raises everything without a rebuild, which is what a support conversation
    // actually needs -- "run it again with this set" beats "install a debug build".
    if (const char* raw = std::getenv("CAD_LOG_LEVEL")) {
        const std::string value(raw);
        if (value == "trace") cad::log::setLevel(Level::Trace);
        else if (value == "debug") cad::log::setLevel(Level::Debug);
        else if (value == "warn") cad::log::setLevel(Level::Warning);
        else if (value == "error") cad::log::setLevel(Level::Error);
        else if (value == "off") cad::log::setLevel(Level::Off);
    }

    const std::string path = resolveLogPath(argv0);
    if (!path.empty()) cad::log::addSink(cad::log::fileSink(path));

    // OCCT last, so its own output lands in a log that already has its sinks.
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
    window.show();

    // Inside the event loop, NOT before it. Creating a document brings up the GPU renderer, and
    // bgfx's Metal backend initialises by handing work to the main run loop and waiting for it:
    // do that on the main thread before app.exec() and the run loop is not being serviced, so the
    // two wait on each other forever. No output, no error, no window -- which is what this mode
    // did when the viewport started rendering for real.
    QTimer::singleShot(0, &app, [&app, &window, path, tab, home] {
        // A populated document, because an empty Home page says nothing about the ribbon and the
        // docks, which is the part being reviewed. `--home` skips it to shoot Home itself.
        if (!home) window.openDemoDocument();
        if (tab >= 0) window.selectRibbonTab(tab);

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
    // Before QApplication, so a failure constructing it is still recorded. Nothing here touches
    // Qt, which is the point: logging is live from the first statement of main.
    const std::string logPath = startLogging(argc > 0 ? argv[0] : nullptr);

    // Shaders sit beside the executable, the same reasoning as the log file above: vCAD ships as
    // a bare binary, and resolving them against the working directory means the viewport draws
    // nothing whenever the app is launched from anywhere but the build directory.
    if (argc > 0 && argv[0] != nullptr && *argv[0] != '\0') {
        std::error_code ec;
        const std::filesystem::path exe =
            std::filesystem::weakly_canonical(std::filesystem::path(argv[0]), ec);
        if (!ec && exe.has_parent_path()) {
            cad::render::setShaderDirectory((exe.parent_path() / "shaders").string());
        }
    }
    CAD_INFO(cad::log::Category::Shell) << "vCAD 0.0.1 starting";
    // Printed to stdout as well, once, because a log nobody can find is a log nobody sends. This is
    // the one line that makes "attach your log" an answerable request.
    if (!logPath.empty()) std::printf("log: %s\n", logPath.c_str());

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
