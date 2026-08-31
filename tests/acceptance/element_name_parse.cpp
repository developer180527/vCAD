/// Reading an element name back from text.
///
/// # Why this is worth its own file
///
/// These strings arrive from saved documents, from DDC cache blobs, and across the C ABI from
/// plugins and from the iPad shell. Some of that is genuinely untrusted input, and the failure that
/// matters is not a crash -- it is a malformed string being reinterpreted as a VALID name that then
/// resolves to real geometry. A reference that silently means the wrong face is the worst failure
/// this layer has, which is the whole premise of ADR 0005.
///
/// So the rule is: `parse` is a strict inverse of `toString`. Anything the writer can produce must
/// read back identically, and anything else must be refused. Every case below is a string the
/// previous parser ACCEPTED, and the name it invented for it.

#include "cad/naming/ElementMap.h"
#include "cad/naming/ElementName.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace cad;
using naming::ElementName;
using naming::NameStep;
using naming::Provenance;

namespace {

ElementName step(std::uint32_t serial, std::uint16_t op, std::uint32_t discriminator,
                 std::vector<std::uint64_t> parents = {}) {
    NameStep s;
    s.featureSerial = serial;
    s.opTag = op;
    s.discriminator = discriminator;
    s.provenance = Provenance::Primitive;
    s.parents = std::move(parents);
    return ElementName({std::move(s)});
}

/// Refused, i.e. parsed to the null name.
bool refused(const std::string& text) { return ElementName::parse(text).isNull(); }

}   // namespace

TEST_CASE("every name the writer produces reads back identically", "[naming][parse]") {
    // The invariant that has to hold, stated on names covering each shape the writer can emit.
    const std::vector<ElementName> names{
        step(1, 0, 0),
        step(4294967295u, 65535u, 4294967295u),           // every field at its maximum
        step(3, 1, 7, {0x0123456789abcdefULL}),
        step(3, 1, 7, {0x0000000000000001ULL, 0xffffffffffffffffULL}),
    };
    for (const auto& name : names) {
        const auto text = name.toString();
        INFO(text);
        CHECK(ElementName::parse(text) == name);
    }

    // A multi-step chain, which is what any element that is not a primitive actually looks like.
    NameStep first;
    first.featureSerial = 1;
    first.provenance = Provenance::Primitive;
    NameStep second;
    second.featureSerial = 2;
    second.provenance = Provenance::Generated;
    second.parents = {0xdeadbeefcafef00dULL};
    const ElementName chained({first, second});
    INFO(chained.toString());
    CHECK(ElementName::parse(chained.toString()) == chained);
}

TEST_CASE("the empty string is the null name, both ways", "[naming][parse]") {
    CHECK(ElementName{}.toString().empty());
    CHECK(ElementName::parse("").isNull());
}

TEST_CASE("a number too large for its field is refused, not truncated", "[naming][parse]") {
    // `sscanf("%llu")` read these happily and the assignment threw the high bits away, so a name
    // naming feature 4294967296 became a name naming feature 0 -- a different, real element.
    CHECK(refused("P4294967296.0#0"));      // featureSerial is 32 bits
    CHECK(refused("P0.65536#0"));           // opTag is 16
    CHECK(refused("P0.0#4294967296"));      // discriminator is 32
    CHECK(refused("P99999999999999999999.0#0"));

    // The maxima themselves still parse: the check is a bound, not an off-by-one.
    CHECK_FALSE(refused("P4294967295.0#0"));
    CHECK_FALSE(refused("P0.65535#0"));
    CHECK_FALSE(refused("P0.0#4294967295"));
}

TEST_CASE("a parent list must be closed, and must not be empty", "[naming][parse]") {
    CHECK(refused("P1.2#3[0123456789abcdef"));    // never terminated
    // Unterminated AND exactly long enough to look right. The list's length is computed on the
    // assumption that the last character is the closing bracket, so with one extra character after
    // sixteen valid hex digits the window lands on those sixteen and the stray one is dropped --
    // a malformed string reading back as a perfectly good name for a real element.
    CHECK(refused("P1.2#3[0123456789abcdefz"));
    CHECK(refused("P1.2#3[]"));                   // the writer never emits an empty list
    CHECK(refused("P1.2#3]"));                    // a bracket with nothing to close
    CHECK(refused("P1.2#3[0123456789abcdef]x"));  // closed, then more
    CHECK_FALSE(refused("P1.2#3[0123456789abcdef]"));
}

TEST_CASE("a parent digest must be exactly the sixteen digits written", "[naming][parse]") {
    // from_chars stops at the first character it cannot use and reports success, so a truncated or
    // corrupted digest was read as a smaller number -- a valid-looking name for a different
    // element.
    CHECK(refused("P1.2#3[0123456789abcdeg]"));    // not hex
    CHECK(refused("P1.2#3[0123456789abcde]"));     // fifteen digits
    CHECK(refused("P1.2#3[0123456789abcdef0]"));   // seventeen
    CHECK(refused("P1.2#3[0123456789abcdef,]"));   // trailing comma
    CHECK(refused("P1.2#3[,0123456789abcdef]"));   // leading comma
    CHECK(refused("P1.2#3[0123456789abcdef,,0123456789abcdef]"));
    CHECK_FALSE(refused("P1.2#3[0123456789abcdef,fedcba9876543210]"));
}

TEST_CASE("the numeric header must be exactly three fields", "[naming][parse]") {
    CHECK(refused("P1.2#3junk"));   // sscanf read three fields and ignored the rest
    CHECK(refused("P1.2"));         // no discriminator
    CHECK(refused("P1#3"));         // no opTag
    CHECK(refused("P1.2#"));        // empty discriminator
    CHECK(refused("P.2#3"));        // empty serial
    CHECK(refused("P1.#3"));        // empty opTag
    CHECK(refused("P#3.1"));        // fields out of order
    CHECK(refused("P1.2#3#4"));
    CHECK(refused("P 1.2#3"));      // the writer emits no spaces
    CHECK(refused("P+1.2#3"));
    CHECK(refused("P-1.2#3"));
}

TEST_CASE("separators are exact: no empty path components", "[naming][parse]") {
    // These all collapsed to the same name, so four different strings meant one element and the
    // format had no single spelling for it.
    CHECK_FALSE(refused("P1.0#0"));
    CHECK(refused("/P1.0#0"));
    CHECK(refused("P1.0#0/"));
    CHECK(refused("P1.0#0//P2.0#0"));
    CHECK(refused("/"));
    CHECK_FALSE(refused("P1.0#0/G2.0#0"));
}

TEST_CASE("an unknown provenance letter is refused", "[naming][parse]") {
    CHECK(refused("X1.2#3"));
    CHECK(refused("p1.2#3"));   // the writer emits upper case
    CHECK(refused("1.2#3"));    // no letter at all
}

TEST_CASE("a refused name cannot resolve to anything", "[naming][parse]") {
    // The consequence that makes the rest of this file matter rather than being pedantry. Callers
    // treat a null name as "this document contains a reference we cannot read"; what must never
    // happen is a malformed string quietly naming a real element.
    naming::ElementMap map;
    CHECK_FALSE(map.resolve(ElementName::parse("P4294967296.0#0")).has_value());
    CHECK_FALSE(map.resolve(ElementName::parse("P1.2#3[0123456789abcde]")).has_value());
}
