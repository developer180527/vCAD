/// Tessellating in parallel must produce exactly what tessellating serially produces.
///
/// `tessellate` is documented as a pure function of a shape and its settings — that claim is what
/// makes the DDC cache legitimate, and it is now also what makes the warm-up safe to run on several
/// threads. If it were not true, the cache was already wrong and the parallelism merely made the
/// wrongness visible sooner.
///
/// So this compares meshes built through the parallel path against meshes built one at a time, and
/// compares the GEOMETRY rather than a count: a mesh with the right number of vertices in the wrong
/// places is exactly what a data race produces.

#include "cad/document/Document.h"
#include "cad/features/Builtins.h"
#include "cad/recompute/DdcCache.h"
#include "cad/recompute/Engine.h"
#include "cad/render/Tessellate.h"
#include "cad/units/Units.h"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <vector>

using namespace cad;

namespace {

/// A document of `n` boxes, every one a different size, so no two share a cache key.
document::Document distinctParts(std::size_t n) {
    document::Document doc;
    for (std::size_t i = 0; i < n; ++i) {
        auto [added, id] = doc.add("Box");
        doc = added.replace(std::make_shared<const document::ObjectData>(
            added.find(id)->withProperty("dx", units::millimetres(10.0 + double(i)))
                .withProperty("dy", units::millimetres(20.0 + double(i) * 0.5))
                .withProperty("dz", units::millimetres(5.0 + double(i) * 0.25))));
    }
    const auto registry = features::builtins();
    recompute::MemoryCache cache;
    recompute::Engine engine(registry, cache);
    auto computed = engine.recompute(doc);
    REQUIRE(computed);
    return computed.value().first;
}

}  // namespace

TEST_CASE("the parallel warm-up builds exactly what the serial path builds", "[render][tessellate]") {
    // Enough parts that the pool actually has work to distribute; a handful would run on one
    // thread and prove nothing about concurrency.
    constexpr std::size_t kParts = 64;
    const document::Document doc = distinctParts(kParts);

    std::vector<const document::Output*> outputs;
    for (const auto id : doc.ids()) {
        const auto object = doc.find(id);
        if (object && object->output() != nullptr) outputs.push_back(object->output());
    }
    REQUIRE(outputs.size() == kParts);

    const render::TessellationSettings settings;

    // Serial: one at a time, through the ordinary path.
    recompute::MemoryBlobStore serialBlobs;
    render::MeshCache serialCache(serialBlobs);
    std::vector<render::RenderMeshPtr> serial;
    for (const auto* output : outputs) {
        auto mesh = serialCache.get(*output, settings);
        REQUIRE(mesh);
        serial.push_back(mesh.value());
    }

    // Parallel: warmed all at once, then read back.
    recompute::MemoryBlobStore parallelBlobs;
    render::MeshCache parallelCache(parallelBlobs);
    const std::size_t built = parallelCache.warm(outputs, settings);
    CHECK(built == kParts);

    // Every subsequent get must be a HIT — if the warm-up did not populate the cache, the
    // comparison below would silently be serial-versus-serial and prove nothing.
    parallelCache.resetStats();
    for (std::size_t i = 0; i < outputs.size(); ++i) {
        auto mesh = parallelCache.get(*outputs[i], settings);
        REQUIRE(mesh);
        const render::RenderMesh& a = *serial[i];
        const render::RenderMesh& b = *mesh.value();

        REQUIRE(a.vertices.size() == b.vertices.size());
        REQUIRE(a.indices.size() == b.indices.size());
        REQUIRE(a.edgeVertices.size() == b.edgeVertices.size());

        // POSITIONS, not counts. A race gives the right number of vertices in the wrong places.
        for (std::size_t v = 0; v < a.vertices.size(); ++v) {
            CHECK(a.vertices[v].position[0] == b.vertices[v].position[0]);
            CHECK(a.vertices[v].position[1] == b.vertices[v].position[1]);
            CHECK(a.vertices[v].position[2] == b.vertices[v].position[2]);
            CHECK(a.vertices[v].element == b.vertices[v].element);
        }
        for (std::size_t k = 0; k < a.indices.size(); ++k) {
            CHECK(a.indices[k] == b.indices[k]);
        }
    }
    CHECK(parallelCache.hits() == kParts);
    CHECK(parallelCache.misses() == 0);
}

TEST_CASE("warming an already-warm cache builds nothing", "[render][tessellate]") {
    const document::Document doc = distinctParts(8);
    std::vector<const document::Output*> outputs;
    for (const auto id : doc.ids()) {
        const auto object = doc.find(id);
        if (object && object->output() != nullptr) outputs.push_back(object->output());
    }

    recompute::MemoryBlobStore blobs;
    render::MeshCache cache(blobs);
    CHECK(cache.warm(outputs, render::TessellationSettings{}) == outputs.size());
    // Idempotent, and cheap. A rebuild that changed no geometry — an orbit, a selection — must not
    // re-tessellate the assembly, which is the difference between a free camera move and a stall.
    CHECK(cache.warm(outputs, render::TessellationSettings{}) == 0);

    // Duplicates within ONE call are deduplicated too: two placements of the same part share a key,
    // and building it twice in parallel is worse than building it once.
    std::vector<const document::Output*> doubled = outputs;
    doubled.insert(doubled.end(), outputs.begin(), outputs.end());
    recompute::MemoryBlobStore freshBlobs;
    render::MeshCache fresh(freshBlobs);
    CHECK(fresh.warm(doubled, render::TessellationSettings{}) == outputs.size());
}
