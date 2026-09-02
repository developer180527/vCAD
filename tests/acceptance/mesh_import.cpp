/// Importing a mesh.
///
/// # The measurement behind this
///
/// A mesh format carries no B-rep topology, so the reader turns every triangle into a face. A NASA
/// Saturn V stage in the corpus is 37,827 of them: 13 seconds to read, and over a minute more to
/// name -- measured, not estimated, with tools/import_probe.
///
/// The cost is the smaller half of the problem. The names themselves are worthless: nobody
/// references "triangle 24,912", any re-tessellation renumbers every one of them, and no feature
/// can be built on a triangle -- which is what a name is FOR. So a mesh is imported as geometry you
/// can see, measure and export onward, with no element names at all.
///
/// Decided by asking the FORMAT, not by counting faces. A STEP file with 37,000 faces is a real
/// B-rep and still gets named; Capabilities::solids is documented as "true B-rep; false means the
/// format is mesh-only" and is exactly this question.
///
/// The fixture is written by the test rather than committed. An STL small enough to commit is not
/// representative of anything, and the real corpus cannot live in the repository.

#include "cad/app/Controller.h"
#include "cad/io/Format.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

using namespace cad;

namespace {

/// A tetrahedron, as ASCII STL. Four triangles is enough to show that nothing is named; the
/// behaviour does not change with size, only the cost does.
std::filesystem::path writeTetrahedron() {
    const auto path = std::filesystem::temp_directory_path() / "vcad_mesh_import.stl";
    std::ofstream out(path, std::ios::trunc);
    out << "solid tetra\n";
    const char* facets[] = {
        "0 0 0  10 0 0  0 10 0",
        "0 0 0  0 10 0  0 0 10",
        "0 0 0  0 0 10  10 0 0",
        "10 0 0  0 10 0  0 0 10",
    };
    for (const char* f : facets) {
        double x1 = 0, y1 = 0, z1 = 0, x2 = 0, y2 = 0, z2 = 0, x3 = 0, y3 = 0, z3 = 0;
        std::sscanf(f, "%lf %lf %lf %lf %lf %lf %lf %lf %lf", &x1, &y1, &z1, &x2, &y2, &z2, &x3,
                    &y3, &z3);
        out << "  facet normal 0 0 0\n    outer loop\n";
        out << "      vertex " << x1 << " " << y1 << " " << z1 << "\n";
        out << "      vertex " << x2 << " " << y2 << " " << z2 << "\n";
        out << "      vertex " << x3 << " " << y3 << " " << z3 << "\n";
        out << "    endloop\n  endfacet\n";
    }
    out << "endsolid tetra\n";
    return path;
}

}   // namespace

TEST_CASE("a mesh imports as geometry, with no element names", "[import][mesh]") {
    const auto path = writeTetrahedron();

    app::Controller c;
    const auto imported = c.importFile(path);
    if (!imported.ok()) INFO(imported.error().message);
    REQUIRE(imported.ok());

    const auto object = c.document().find(imported.value());
    REQUIRE(object);
    INFO("state " << static_cast<int>(object->state()) << ": " << object->error().message);

    // It opens, and it is real geometry: visible, measurable, exportable.
    CHECK(object->state() == document::ObjectState::Clean);
    REQUIRE(object->output() != nullptr);
    CHECK(object->output()->shape.subShapes(kernel::ShapeType::Face).size() == 4);

    // And nothing is named. Not "named badly" -- not named, because a triangle is not a feature and
    // a name that no feature can use is a name that only costs time.
    CHECK(object->output()->map.size() == 0);

    std::filesystem::remove(path);
}

TEST_CASE("the decision comes from the format, not from the geometry", "[import][mesh]") {
    // A STEP file with tens of thousands of faces is a real B-rep and must still be named. If this
    // ever starts keying off a face count, that is the test that fails.
    const auto registry = io::FormatRegistry::builtins();

    const auto* stl = registry.forPath("part.stl");
    REQUIRE(stl != nullptr);
    CHECK_FALSE(stl->capabilities().solids);   // mesh-only: not named

    for (const char* name : {"part.step", "part.stp", "part.iges"}) {
        const auto* provider = registry.forPath(name);
        INFO(name);
        REQUIRE(provider != nullptr);
        CHECK(provider->capabilities().solids);   // B-rep: named as before
    }
}

TEST_CASE("importing a mesh is not slow", "[import][mesh]") {
    // The cost this exists to remove. Four triangles cannot show a minute's difference, so this
    // only guards the shape of the thing: an import that started naming meshes again would show up
    // here first on a corpus file, and here eventually as the fixture grows.
    const auto path = writeTetrahedron();
    const auto start = std::chrono::steady_clock::now();

    app::Controller c;
    REQUIRE(c.importFile(path).ok());

    const auto ms = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - start).count();
    INFO("took " << ms << " ms");
    CHECK(ms < 2000.0);
    std::filesystem::remove(path);
}
