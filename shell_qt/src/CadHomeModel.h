#pragma once

/// vCAD's answers to the four questions `proshell::HomePage` asks.
///
/// The whole of what used to couple the home page to CAD, and it comes to about eighty lines:
/// the product name, the four document kinds, the recent list, and what to say about the project
/// and the shared cache. The page's layout, card grid, search, sort, empty state and rail are
/// proshell's and appear nowhere here.

#include "cad/app/Session.h"
#include "proshell/HomeModel.h"

namespace cadqt {

class CadHomeModel : public proshell::HomeModel {
public:
    explicit CadHomeModel(cad::app::Session& session) : session_(session) {}

    [[nodiscard]] QString productName() const override;
    [[nodiscard]] QString productDetail() const override;
    [[nodiscard]] std::vector<proshell::DocumentKind> documentKinds() const override;
    [[nodiscard]] std::vector<proshell::RecentDocument> recent() const override;
    [[nodiscard]] std::vector<proshell::SummaryField> summary() const override;

private:
    cad::app::Session& session_;
};

}  // namespace cadqt
