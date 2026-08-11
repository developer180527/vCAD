#include "Model.h"
#include <catch2/catch_test_macros.hpp>
#include <cstdio>
using namespace cadtest;
using cad::kernel::BoxFace;

static void dumpFaces(const char* label, const Model& m) {
    std::printf("== %s\n", label);
    for (const auto& f : m.shape.subShapes(cad::kernel::ShapeType::Face)) {
        const auto q = f.measure();
        const auto n = m.map.nameOf(f);
        std::printf("  face area=%9.2f c=(%7.2f,%7.2f,%7.2f)  name=%s\n",
                    q.mass, q.cx, q.cy, q.cz, n ? n->toString().c_str() : "<UNNAMED>");
    }
}

// Diagnostic, not an assertion: prints every face with its measure and assigned name.
// Hidden by default (leading dot); run with:  cad_tests "[debug]"
TEST_CASE("debug dump", "[.][debug]") {
    auto base = box(100.0, 60.0, 40.0);
    std::printf("top=%s front=%s\n",
                faceName(base.value(), BoxFace::ZMax).value().toString().c_str(),
                faceName(base.value(), BoxFace::YMin).value().toString().c_str());
    dumpFaces("BASE", base.value());

    auto t2 = box(140.0, 30.0, 30.0, 10);
    dumpFaces("TOOLBOX (pre-translate)", t2.value());
    auto tool2 = translated(t2.value(), -20.0, -10.0, 30.0);
    dumpFaces("TOOL (translated)", tool2.value());
    auto chopped = cut(base.value(), tool2.value());
    REQUIRE(chopped.ok());
    dumpFaces("CHOPPED", chopped.value());
}
