// The application and the C ABI must compute the same thing from the same document.
//
// # Why there are two paths at all
//
// `app::Controller` is what both shells run. `abi::Session` (abi/src/Session.cpp) is what the Rust
// suite drives -- concurrency, property invariants, the torture test, scaling, rollback,
// persistence. Each independently builds its own feature registry, recompute engine, cache and
// scene builder over the same core. Neither is layered on the other.
//
// That happened because the ABI was designed with the iPad as its second consumer (the header's own
// intro says so), and the iPad was then built on the C++ Controller instead. Nothing recorded the
// change: ADR 0011, the ABI's decision record, does not mention the iPad at all.
//
// # What this protects
//
// The deepest tests in the project exercise the sibling, not the product. They are only evidence
// about the shipping application for as long as the two compute the same answers, and until now
// nothing checked that they did.
//
// # Why a round trip rather than two builds
//
// Building the same model twice does not give comparable results: a new Controller seeds its own
// origin objects, so ids differ, and element names carry the id as their feature serial. That is
// a legitimate difference, not a divergence. A SAVED DOCUMENT fixes the ids. So each direction
// builds through one path, saves, opens through the other, and compares object by object.
//
// Content hash is the main comparison because it covers both geometry and naming (see
// naming::contentHash): two paths that agree on shape but disagree on a single element name fail.

#include "cad/abi/cad_plugin_abi.h"
#include "cad/app/Controller.h"
#include "cad/naming/ElementMap.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using cad::app::Controller;

namespace {

/// A file in the temp directory, removed when the test ends however it ends.
struct TempFile {
    explicit TempFile(const std::string& name) : path(fs::temp_directory_path() / name) {
        fs::remove(path);
    }
    ~TempFile() {
        std::error_code ignored;
        fs::remove(path, ignored);
    }
    fs::path path;
};

/// An ABI session, released when the test ends.
struct Session {
    Session() : handle(cad_session_create()) {}
    ~Session() { cad_session_release(handle); }
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    CadSession handle;
};

/// What one path says about one object.
struct Answer {
    bool computed = false;
    std::string contentHash;
    double volume = 0.0;
    std::uint64_t faces = 0;
    std::uint64_t edges = 0;
};

Answer throughAbi(CadSession s, CadObject id) {
    Answer a;
    double volume = 0.0;
    if (cad_object_volume(s, id, &volume) != CAD_OK) return a;   // no geometry
    a.computed = true;
    a.volume = volume;
    a.contentHash = cad_object_content_hash(s, id);
    cad_object_face_count(s, id, &a.faces);
    cad_object_edge_count(s, id, &a.edges);
    return a;
}

Answer throughApp(const Controller& app, cad::document::ObjectId id) {
    Answer a;
    const auto object = app.document().find(id);
    if (!object || object->output() == nullptr) return a;
    const auto& output = *object->output();
    const double volume = output.shape.volume();
    if (!std::isfinite(volume)) return a;
    a.computed = true;
    a.volume = volume;
    // The same function the ABI calls, over the same inputs -- so a difference here is a difference
    // in what was COMPUTED, not in how it was summarised.
    if (const auto hash = cad::naming::contentHash(output.shape, output.map); hash.ok()) {
        a.contentHash = hash.hex();
    }
    a.faces = output.shape.subShapes(cad::kernel::ShapeType::Face).size();
    a.edges = output.shape.subShapes(cad::kernel::ShapeType::Edge).size();
    return a;
}

void requireAgree(const Answer& abi, const Answer& app, const std::string& what) {
    INFO(what << ": ABI " << (abi.computed ? "computed" : "did not compute") << ", app "
              << (app.computed ? "computed" : "did not compute"));
    REQUIRE(abi.computed);
    REQUIRE(app.computed);

    INFO("volume ABI " << abi.volume << ", app " << app.volume);
    CHECK(abi.volume == Catch::Approx(app.volume).epsilon(1e-9));
    CHECK(abi.faces == app.faces);
    CHECK(abi.edges == app.edges);

    // Not empty, or two unhashable shapes would "agree" on nothing.
    INFO("content hash ABI " << abi.contentHash << "\n             app " << app.contentHash);
    CHECK_FALSE(abi.contentHash.empty());
    CHECK(abi.contentHash == app.contentHash);
}

const cad::app::Command* commandNamed(const Controller& app, const std::string& id) {
    for (const auto& command : app.commands()) {
        if (command.id == id) return &command;
    }
    return nullptr;
}

}  // namespace

TEST_CASE("a document built through the ABI computes identically in the app", "[abi][app][conformance]") {
    TempFile file("vcad_two_paths_abi_to_app.vpart");

    // A box, a cylinder overlapping its corner, and a Cut of one by the other. Three objects and a
    // dependency: enough that naming PROPAGATES through a boolean rather than only being assigned to
    // primitives, which is where two independently wired paths are likeliest to differ.
    CadObject box = 0, cylinder = 0, cut = 0;
    {
        Session s;
        REQUIRE(cad_object_add(s.handle, "Box", &box) == CAD_OK);
        REQUIRE(cad_object_set_length(s.handle, box, "dx", 100.0) == CAD_OK);
        REQUIRE(cad_object_set_length(s.handle, box, "dy", 60.0) == CAD_OK);
        REQUIRE(cad_object_set_length(s.handle, box, "dz", 40.0) == CAD_OK);

        REQUIRE(cad_object_add(s.handle, "Cylinder", &cylinder) == CAD_OK);
        REQUIRE(cad_object_set_length(s.handle, cylinder, "radius", 25.0) == CAD_OK);
        REQUIRE(cad_object_set_length(s.handle, cylinder, "height", 80.0) == CAD_OK);

        // The property names the app's own addBoolean uses, which order the inputs.
        REQUIRE(cad_object_add(s.handle, "Cut", &cut) == CAD_OK);
        REQUIRE(cad_object_set_object(s.handle, cut, "a_base", box) == CAD_OK);
        REQUIRE(cad_object_set_object(s.handle, cut, "b_tool", cylinder) == CAD_OK);

        CadRecomputeReport report{};
        REQUIRE(cad_recompute(s.handle, &report) == CAD_OK);
        REQUIRE(report.failed == 0);
        REQUIRE(cad_document_save(s.handle, file.path.string().c_str()) == CAD_OK);
    }

    // Fresh sessions on both sides, so neither answer can come from a cache the build left warm.
    Session abi;
    REQUIRE(cad_document_open(abi.handle, file.path.string().c_str()) == CAD_OK);

    Controller app;
    REQUIRE(app.loadFrom(file.path));

    requireAgree(throughAbi(abi.handle, box), throughApp(app, cad::document::ObjectId{box}), "Box");
    requireAgree(throughAbi(abi.handle, cylinder),
                 throughApp(app, cad::document::ObjectId{cylinder}), "Cylinder");
    requireAgree(throughAbi(abi.handle, cut), throughApp(app, cad::document::ObjectId{cut}), "Cut");
}

TEST_CASE("a document built through the app computes identically through the ABI", "[abi][app][conformance]") {
    // The other direction, built the way a user builds it: commands, a selection, then Cut. A
    // one-way check would only prove the app can read what the ABI writes.
    TempFile file("vcad_two_paths_app_to_abi.vpart");

    std::vector<cad::document::ObjectId> ids;
    {
        Controller app;
        for (const char* id : {"feature.box", "feature.cylinder"}) {
            const auto* command = commandNamed(app, id);
            REQUIRE(command != nullptr);
            command->invoke();
            app.refresh();
            REQUIRE(app.selection().size() == 1);
            ids.push_back(app.selection().front());
        }
        app.setSelection({ids[0], ids[1]});
        const auto* cut = commandNamed(app, "feature.cut");
        REQUIRE(cut != nullptr);
        cut->invoke();
        app.refresh();
        REQUIRE(app.selection().size() == 1);
        ids.push_back(app.selection().front());
        REQUIRE(app.saveTo(file.path));
    }

    Controller app;
    REQUIRE(app.loadFrom(file.path));
    Session abi;
    REQUIRE(cad_document_open(abi.handle, file.path.string().c_str()) == CAD_OK);

    const char* names[] = {"Box", "Cylinder", "Cut"};
    for (std::size_t i = 0; i < ids.size(); ++i) {
        requireAgree(throughAbi(abi.handle, ids[i].value), throughApp(app, ids[i]), names[i]);
    }
}
