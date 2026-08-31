// Custom Catch2 main: we need argv[0] for the cross-process determinism test, and an
// early-exit path so the child process can emit its digest without running the suite.

#include "Model.h"
#include "cad/kernel/Booleans.h"
#include "cad/kernel/Transform.h"
#include "cad/naming/ElementMap.h"

#include <catch2/catch_session.hpp>

#include <cstdlib>
#include <iostream>
#include <sstream>
#include <algorithm>
#include <string>
#include <vector>

namespace cadtest {

std::string g_argv0;

/// The canonical description of a fixed model: every element name, plus the content hash.
/// Must be byte-identical in any process on any machine.
std::string emitDigest() {
    std::ostringstream os;

    auto b = box(100.0, 60.0, 40.0);
    if (!b) return {};
    auto edge = edgeBetween(b.value(), cad::kernel::BoxFace::ZMax, cad::kernel::BoxFace::YMin);
    if (!edge) return {};
    auto f = fillet(b.value(), edge.value(), 5.0);
    if (!f) return {};

    for (const auto& n : nameStrings(f.value())) os << n << '\n';
    os << cad::naming::contentHash(f.value().shape, f.value().map).hex() << '\n';

    // A model with SPLIT FACES in it, because the fillet above has none.
    //
    // Two overlapping boxes fused: the overlap cuts faces in half, and each half is numbered by
    // canonical measure order. That numbering is the one part of split naming that cannot be
    // checked within a single process -- OCCT's traversal order is stable inside one run, so a
    // build that dropped the ordering entirely would still agree with itself. Putting the model
    // here at least holds the names fixed across processes, and pins them against accidental
    // change.
    auto left = box(40.0, 30.0, 20.0, 1);
    auto right = box(40.0, 30.0, 20.0, 2);
    if (!left || !right) return {};
    auto shifted = cad::kernel::translate(right.value().shape, 20.0, 0.0, 0.0);
    if (!shifted) return {};
    cad::naming::NamingContext moveCtx(2, 0);
    auto shiftedMap = moveCtx.propagate(shifted.value(), {&right.value().shape},
                                        {&right.value().map});
    if (!shiftedMap) return {};
    const cad::kernel::Shape shiftedShape = shifted.value().shape();

    auto fused = cad::kernel::booleanFuse(left.value().shape, shiftedShape);
    if (!fused) return {};
    cad::naming::NamingContext fuseCtx(3, 0);
    auto fusedMap = fuseCtx.propagate(fused.value(), {&left.value().shape, &shiftedShape},
                                      {&left.value().map, &shiftedMap.value()});
    if (!fusedMap) return {};

    std::vector<std::string> split;
    for (const auto& n : fusedMap.value().allNames()) split.push_back(n.toString());
    std::sort(split.begin(), split.end());
    for (const auto& n : split) os << n << '\n';
    os << cad::naming::contentHash(fused.value().shape(), fusedMap.value()).hex() << '\n';
    return os.str();
}

}  // namespace cadtest

int main(int argc, char** argv) {
    cadtest::g_argv0 = argc > 0 ? argv[0] : "";

    if (std::getenv("CAD_EMIT_DIGEST") != nullptr) {
        const std::string out = cadtest::emitDigest();
        if (out.empty()) return 1;
        std::cout << out;
        return 0;
    }

    return Catch::Session().run(argc, argv);
}
