/// Naming serials for shapes the host builds outside a feature compute.
///
/// # What is actually at stake
///
/// A host-built shape's naming serial is derived from the request, so that identical calls name
/// identically. It has to be folded to 32 bits, because that is the width of NameStep::featureSerial
/// and that field goes into saved documents. The birthday bound on 32 bits is around 2^16 distinct
/// requests, which a long-running plugin session reaches -- and a collision does not look like a
/// failure. It looks like two different shapes whose faces answer to the same names, so a reference
/// to one is a perfectly valid reference to the other.
///
/// Reached by relative path into abi/src, like the plugin manifest parser. A real 32-bit collision
/// cannot be produced by luck in a test, so the fold is exercised directly with fingerprints chosen
/// to land on the same serial. That is the only way to see this code do its job.

#include "../../abi/src/SerialLedger.h"

#include <catch2/catch_test_macros.hpp>

using cad::abi::SerialLedger;

namespace {

/// A fingerprint whose high and low halves are given, so a caller can aim the fold.
constexpr std::uint64_t fingerprint(std::uint32_t high, std::uint32_t low) {
    return (static_cast<std::uint64_t>(high) << 32) | low;
}

}   // namespace

TEST_CASE("the same request always gets the same serial", "[abi][serial]") {
    // The property the whole derivation exists for. A plugin that builds the same box twice must
    // get the same names both times, or nothing it references survives a rebuild.
    SerialLedger ledger;
    const auto first = ledger.serialFor(fingerprint(0x1234, 0x5678));
    const auto again = ledger.serialFor(fingerprint(0x1234, 0x5678));
    REQUIRE(first.has_value());
    REQUIRE(again.has_value());
    CHECK(*first == *again);
    CHECK(ledger.size() == 1);
}

TEST_CASE("different requests normally get different serials", "[abi][serial]") {
    SerialLedger ledger;
    const auto a = ledger.serialFor(fingerprint(1, 2));
    const auto b = ledger.serialFor(fingerprint(1, 3));
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    CHECK(*a != *b);
}

TEST_CASE("a serial claimed by another request is refused", "[abi][serial]") {
    // Two fingerprints that fold to the same 32 bits: 1^2 and 3^0 are both 3. This is the case that
    // cannot be reached by chance in a test and will be reached by chance in a long session.
    SerialLedger ledger;
    const auto first = ledger.serialFor(fingerprint(1, 2));
    REQUIRE(first.has_value());
    CHECK(*first == 3u);

    const auto colliding = ledger.serialFor(fingerprint(3, 0));
    CHECK_FALSE(colliding.has_value());

    // And the refusal does not disturb the request that got there first: asking again still works,
    // so one unlucky plugin call cannot poison every later one.
    const auto original = ledger.serialFor(fingerprint(1, 2));
    REQUIRE(original.has_value());
    CHECK(*original == 3u);
}

TEST_CASE("a serial is never zero", "[abi][serial]") {
    // Zero is what an uninitialised serial looks like, so a name carrying it would collide with
    // anything built before its serial was set -- invisibly, rather than merely wrongly.
    SerialLedger ledger;
    const auto folded = ledger.serialFor(fingerprint(0x89abcdef, 0x89abcdef));   // xor is 0
    REQUIRE(folded.has_value());
    CHECK(*folded != 0u);
}

TEST_CASE("the ledger grows with distinct serials, not with calls", "[abi][serial]") {
    SerialLedger ledger;
    for (int i = 1; i <= 50; ++i) {
        (void)ledger.serialFor(fingerprint(0, static_cast<std::uint32_t>(i)));
        (void)ledger.serialFor(fingerprint(0, static_cast<std::uint32_t>(i)));   // repeat
    }
    CHECK(ledger.size() == 50);
}

TEST_CASE("the never-zero clamp is itself a collision, and is reported as one", "[abi][serial]") {
    // Found by a test that started counting at zero rather than one.
    //
    // A fingerprint whose halves cancel folds to 0, and 0 is remapped to 1 so that a name can never
    // carry an uninitialised-looking serial. That remapping puts it on top of whatever genuinely
    // folds to 1 -- so serial 1 has two ways to be claimed and is the one value more likely to
    // collide than chance suggests.
    //
    // It is caught by the same check as any other collision, which is the point: the clamp does not
    // need its own special case, it needs to not be silent.
    SerialLedger ledger;
    const auto genuine = ledger.serialFor(fingerprint(0, 1));
    REQUIRE(genuine.has_value());
    CHECK(*genuine == 1u);

    const auto clamped = ledger.serialFor(fingerprint(7, 7));   // xor is 0, remapped to 1
    CHECK_FALSE(clamped.has_value());
}
