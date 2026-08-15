// The Rust static library, called from C++.
//
// This is the whole point of the stub: PICKUP.md's step 2 says the risky part of a Rust parser is
// the BUILD INTEGRATION, not the parsing, and that it should be proven on a trivial function
// before any DXF code exists. A test that links the archive and calls through it is what turns
// "cmake configured successfully" into evidence — configuration proves cargo was found, and
// nothing more. Symbol naming, the static archive's link order, Rust's std requirements on this
// platform, and the calling convention are all only exercised by an actual call.
//
// Deliberately in the C++ acceptance suite rather than in tests-rs. The Rust suite runs cargo,
// which would test the crate against itself; this runs the linker, which is the thing that breaks.

#include <catch2/catch_test_macros.hpp>

#include <cad_parse.h>

#include <cstring>

TEST_CASE("the Rust library links and answers", "[rust]") {
    // If the archive were missing, stale, or built for another architecture, the failure is at
    // link time and this test never runs — which is itself the report. What this catches is the
    // subtler case: it linked, and the halves disagree.
    REQUIRE(cad_parse_abi_version() == CAD_PARSE_ABI_EXPECTED);
}

TEST_CASE("the Rust library returns a string through a caller-owned buffer", "[rust]") {
    // A version number alone cannot distinguish "the integration works" from "the linker found an
    // old archive that happens to return 1". A string proves the running code came from this
    // source, and exercises the buffer contract at the same time.
    char buffer[64] = {};
    const std::size_t written = cad_parse_build_id(buffer, sizeof(buffer));

    REQUIRE(written > 0);
    REQUIRE(written < sizeof(buffer));
    REQUIRE(buffer[written] == '\0');
    REQUIRE(std::strlen(buffer) == written);
    REQUIRE(std::strstr(buffer, "cad-parse") != nullptr);
}

TEST_CASE("the Rust library refuses a buffer it would overflow", "[rust]") {
    // The boundary rule that matters most, checked from the side that owns the memory. A parser
    // that truncates into a short buffer is a parser that will one day write past it, and the
    // whole argument for moving parsing to Rust is that this class of bug stops being possible.
    char tiny[4] = {'x', 'x', 'x', 'x'};
    REQUIRE(cad_parse_build_id(tiny, sizeof(tiny)) == 0);
    // Untouched: refused, not partially written.
    REQUIRE(tiny[0] == 'x');
    REQUIRE(tiny[3] == 'x');

    REQUIRE(cad_parse_build_id(nullptr, 64) == 0);
}
