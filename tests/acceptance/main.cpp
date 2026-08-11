// Custom Catch2 main: we need argv[0] for the cross-process determinism test, and an
// early-exit path so the child process can emit its digest without running the suite.

#include "Model.h"
#include "cad/naming/ElementMap.h"

#include <catch2/catch_session.hpp>

#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>

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
