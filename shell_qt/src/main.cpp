#include "MainWindow.h"
#include "Theme.h"

#include <QApplication>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("vCAD"));
    app.setOrganizationName(QStringLiteral("vCAD"));

    cadqt::applyTheme(app);

    cadqt::MainWindow window;
    window.show();
    return app.exec();
}
