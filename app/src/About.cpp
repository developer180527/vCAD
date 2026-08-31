#include "cad/app/About.h"

#include "cad/abi/cad_plugin_abi.h"

#include <Standard_Version.hxx>
#include <Eigen/Core>

namespace cad::app {

std::vector<AboutEntry> about() {
    std::vector<AboutEntry> out;

    // The application's own version, from the build rather than from a literal here: two places to
    // change it is one place to forget.
    out.push_back({"vCAD", CAD_VERSION});
    out.push_back({"Build", CAD_BUILD_TYPE});

    // The geometry kernel, from ITS OWN header. A hard-coded "8.0" would keep saying 8.0 after the
    // day someone upgrades, which is precisely when the number starts to matter.
    out.push_back({"Geometry kernel", std::string("Open CASCADE ") + OCC_VERSION_COMPLETE});

    // The 2D solver is vendored, so it has no version of its own to report — the honest identifier
    // is the upstream commit it was taken from, which modules/planegcs/VENDORED.md records and the
    // build passes through.
    out.push_back({"Sketch solver", std::string("FreeCAD planegcs ") + CAD_PLANEGCS_COMMIT});

    out.push_back({"Linear algebra",
                   "Eigen " + std::to_string(EIGEN_WORLD_VERSION) + "."
                       + std::to_string(EIGEN_MAJOR_VERSION) + "."
                       + std::to_string(EIGEN_MINOR_VERSION)});

    // The plugin ABI, because a plugin author's first question is which one they may target.
    out.push_back({"Plugin ABI", std::to_string(CAD_ABI_VERSION_MAJOR) + "."
                                     + std::to_string(CAD_ABI_VERSION_MINOR)});

    out.push_back({"Document format", "vCAD native (ADR 0003)"});
    return out;
}

}   // namespace cad::app
