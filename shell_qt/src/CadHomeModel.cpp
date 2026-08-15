#include "CadHomeModel.h"

#include <QCoreApplication>

namespace cadqt {

using cad::app::DocumentKind;

QString CadHomeModel::productName() const {
    return QCoreApplication::translate("CadHomeModel", "vCAD 0.1");
}

QString CadHomeModel::productDetail() const {
    return QCoreApplication::translate("CadHomeModel", "M3 · renderer");
}

std::vector<proshell::DocumentKind> CadHomeModel::documentKinds() const {
    // Part first: it is the everyday kind, and the New button's plain click creates the first
    // available one.
    //
    // The unimplemented three are listed rather than hidden (ADR 0009 decision 2). proshell greys
    // them and marks them "not yet" — the page owns how that is presented, this owns which kinds
    // exist and whether each works.
    std::vector<proshell::DocumentKind> kinds;
    for (const auto kind : {DocumentKind::Part, DocumentKind::Assembly, DocumentKind::Drawing,
                            DocumentKind::Presentation}) {
        const QString label = QString::fromUtf8(cad::app::toString(kind));
        kinds.push_back({static_cast<int>(kind), label, label.toLower(),
                         cad::app::implemented(kind)});
    }
    return kinds;
}

std::vector<proshell::RecentDocument> CadHomeModel::recent() const {
    std::vector<proshell::RecentDocument> entries;
    const auto& paths = session_.recent();
    entries.reserve(paths.size());
    for (const auto& path : paths) {
        // Every recent document gets the part glyph, because a part is the only kind that can be
        // saved today. It becomes per-kind the moment another one can be, and the page already
        // takes the icon name per entry so that needs no change there.
        entries.push_back({QString::fromStdString(path.string()), QStringLiteral("part")});
    }
    return entries;
}

std::vector<proshell::SummaryField> CadHomeModel::summary() const {
    const auto& project = session_.project();
    const auto tr = [](const char* text) {
        return QCoreApplication::translate("CadHomeModel", text);
    };

    std::vector<proshell::SummaryField> fields;
    fields.push_back(
        {project.loaded() ? tr("Project: %1").arg(QString::fromStdString(project.name))
                          : tr("Project: none"),
         false, false});
    fields.push_back({tr("Search paths: %1 configured").arg(project.searchPaths.size()),
                      false, false});

    // Honest about the cache: "online" would be a claim about a directory we have not checked.
    // Until the DDC reports its own status this says what is CONFIGURED, not what is reachable.
    if (project.sharedCache.empty()) {
        fields.push_back({tr("○ Shared cache not configured"), true, false});
    } else {
        fields.push_back(
            {tr("● Shared cache: %1").arg(QString::fromStdString(project.sharedCache.string())),
             true, true});
    }
    return fields;
}

}  // namespace cadqt
