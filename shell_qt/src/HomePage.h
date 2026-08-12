#pragma once

#include "cad/app/Session.h"

#include <QWidget>

class QListWidget;

namespace cadqt {

/// Inventor's Home: recent documents, a New panel, the active project.
///
/// Not a document (ADR 0009): it has no file, cannot be closed, and contributes no ribbon tabs.
/// Modelling it as one would mean every "for each open document" loop carries a special case
/// forever.
class HomePage : public QWidget {
    Q_OBJECT
public:
    explicit HomePage(cad::app::Session& session, QWidget* parent = nullptr);
    void refresh();

signals:
    void createRequested(int kind);
    void openRequested(const QString& path);

private:
    cad::app::Session& session_;
    QListWidget* recent_ = nullptr;
};

}  // namespace cadqt
