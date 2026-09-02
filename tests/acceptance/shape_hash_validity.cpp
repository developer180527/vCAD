// A content hash that could not be computed, and the two caches that key on one.
//
// # The hole this closes
//
// `naming::contentHash` reaches OCCT — it walks explorers and takes mass properties on every
// element — and returned a plain `ShapeHash` with no way to say "I could not". It was also
// unguarded, so an unhashable shape ended the process. Guarding it is only half the fix: the
// failure then has to be REPRESENTED, and it cannot be represented as a value, because every
// 256-bit pattern is a legitimate hash. Any sentinel lanes we picked would be a real key that a
// second unhashable shape also lands on.
//
// Two caches key on this hash, and they fail differently:
//
//   * the DDC, via MeshCache — serves one part's mesh for another, from disk, and across machines
//     through the shared tier;
//   * the render backend, which interns GPU buffers by the same hash — draws the wrong mesh, which
//     looks like a rendering fault rather than a cache fault and is the harder one to trace.
//
// So the flag exists, and the tests below pin the two behaviours that make it worth having: an
// invalid hash matches nothing, and nothing dedupes on it.

#include "cad/kernel/Shape.h"
#include "cad/render/NullBackend.h"

#include <catch2/catch_test_macros.hpp>

using cad::kernel::ShapeHash;

namespace {

ShapeHash aHash(std::uint64_t seed) {
    ShapeHash h;
    h.lanes[0] = seed;
    h.lanes[1] = seed ^ 0x1111111111111111ULL;
    h.lanes[2] = seed ^ 0x2222222222222222ULL;
    h.lanes[3] = seed ^ 0x3333333333333333ULL;
    return h;
}

ShapeHash unhashable() {
    ShapeHash h;
    h.valid = false;
    return h;
}

}  // namespace

TEST_CASE("a hash that could not be computed equals nothing", "[hash][cache]") {
    // Including ITSELF. The defaulted comparison would have made two failures compare equal, which
    // reads as "these are the same shape" — the wrong answer, arriving through the operator most
    // likely to be used without thinking about it.
    const ShapeHash failed = unhashable();
    CHECK_FALSE(failed == failed);
    CHECK_FALSE(failed == unhashable());
    CHECK_FALSE(failed == aHash(7));
    CHECK_FALSE(aHash(7) == failed);

    // And a real hash is unaffected: the flag must not disturb the comparison it exists beside.
    CHECK(aHash(7) == aHash(7));
    CHECK_FALSE(aHash(7) == aHash(8));
}

TEST_CASE("a valid hash still reports itself valid", "[hash][cache]") {
    // The default matters: contentHash builds its result by assignment into a default-constructed
    // ShapeHash, so a flag defaulting the wrong way would mark every hash in the system invalid and
    // silently disable both caches — which would show up as a performance problem, not a
    // correctness one, and would be hunted in the wrong place.
    CHECK(ShapeHash{}.ok());
    CHECK(aHash(1).ok());
    CHECK_FALSE(unhashable().ok());
}

TEST_CASE("unhashable meshes are never deduped onto each other", "[hash][cache][render]") {
    // The GPU half. Two different meshes that could not be hashed carry identical lanes, so an
    // intern keyed on content would hand the second one the FIRST one's buffer. Nothing reports
    // it; the viewport simply draws the wrong part.
    cad::render::NullBackend backend;
    auto& gpu = backend.resources;

    const std::vector<cad::render::CadVertex> vertices(3);
    const auto first = gpu.uploadVertices(unhashable(), vertices);
    const auto second = gpu.uploadVertices(unhashable(), vertices);

    REQUIRE(first != cad::render::BufferId::None);
    REQUIRE(second != cad::render::BufferId::None);
    CHECK(first != second);
}

TEST_CASE("identical valid hashes still dedupe", "[hash][cache][render]") {
    // The other half, and the reason the fix is a branch rather than "stop deduping". Dedupe by
    // content is what makes the 50,000th identical bolt free (ADR 0007), so refusing it for every
    // mesh would be a much larger regression than the bug being fixed.
    cad::render::NullBackend backend;
    auto& gpu = backend.resources;

    const std::vector<cad::render::CadVertex> vertices(3);
    const auto first = gpu.uploadVertices(aHash(42), vertices);
    const auto second = gpu.uploadVertices(aHash(42), vertices);
    CHECK(first == second);

    // And two different shapes do not.
    CHECK(gpu.uploadVertices(aHash(43), vertices) != first);
}

TEST_CASE("fold64 and hex do NOT carry validity, and must not be compared", "[hash][cache]") {
    // The trap, pinned rather than the fix.
    //
    // `operator==` was written to make an invalid hash equal nothing, including itself. Every
    // consumer that reaches past it -- to `fold64()` for a key, or to `hex()` for a string -- loses
    // that protection silently, because both are perfectly well-defined on an invalid hash and
    // neither looks dangerous at the call site.
    //
    // Four places in the C ABI did exactly that and were found by reading, not by a failing test:
    // a naming serial derived from folded input hashes, a determinism self-check comparing folds, a
    // volume accessor, and a content-hash accessor. This states the property that makes all four
    // wrong, so the next one is caught here instead.
    cad::kernel::ShapeHash first;
    first.valid = false;
    cad::kernel::ShapeHash second;
    second.valid = false;

    // Unequal by the operator, as designed.
    CHECK_FALSE(first == second);
    CHECK_FALSE(first == first);

    // And EQUAL by their fold, which is the whole problem: two shapes nobody could identify produce
    // one key, so anything keyed on the fold treats them as the same shape.
    CHECK(first.fold64() == second.fold64());

    // The same for the string form: not empty, not obviously wrong, and shared by every failure.
    CHECK(first.hex() == second.hex());
    CHECK(first.hex().size() == 64);
}
