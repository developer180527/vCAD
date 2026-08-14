// The size-prefix rule, enforced by the compiler rather than by review.
//
// ADR 0011's whole compatibility mechanism is that a receiver reads `struct_size` and uses only
// the fields that fit inside it. That works if and only if `struct_size` is genuinely the first
// member of every descriptor, at offset 0, with `struct_version` immediately after. If someone
// ever appends a field ABOVE struct_size — or adds a descriptor and forgets the prefix entirely —
// the negotiation reads a garbage size from whatever now sits at offset 0, and the failure appears
// as memory corruption inside a third-party plugin.
//
// These are static_asserts on purpose: a runtime test can only fail after someone runs it, while
// this fails the build. It is the cheapest possible enforcement of the most expensive possible
// mistake.

#include "cad/abi/cad_plugin_abi.h"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>

namespace {

// --- the size prefix, on every descriptor that crosses the boundary ---------------------

#define CAD_ASSERT_SIZE_PREFIXED(T)                                                          \
    static_assert(offsetof(T, struct_size) == 0,                                             \
                  #T " must begin with struct_size; the ABI negotiation reads offset 0");    \
    static_assert(offsetof(T, struct_version) == sizeof(uint32_t),                           \
                  #T " must carry struct_version immediately after struct_size");            \
    static_assert(sizeof(T) >= 2 * sizeof(uint32_t), #T " is impossibly small")

CAD_ASSERT_SIZE_PREFIXED(CadFeatureDesc);
CAD_ASSERT_SIZE_PREFIXED(CadCommandDesc);
CAD_ASSERT_SIZE_PREFIXED(CadFormatDesc);
CAD_ASSERT_SIZE_PREFIXED(CadParamDesc);
CAD_ASSERT_SIZE_PREFIXED(CadPluginDesc);
CAD_ASSERT_SIZE_PREFIXED(CadHost);

#undef CAD_ASSERT_SIZE_PREFIXED

// --- parameter kinds are STORED IN DOCUMENTS and are therefore permanent -----------------
//
// Reordering the enum would silently change what every saved file means: a Length becomes an
// Angle, and a 40 mm extrude becomes a 40 degree one. Pinned here so that reordering fails the
// build rather than corrupting documents in the field.

static_assert(CAD_PARAM_LENGTH == 0, "CadParamKind values are stored in documents");
static_assert(CAD_PARAM_ANGLE == 1, "CadParamKind values are stored in documents");
static_assert(CAD_PARAM_REAL == 2, "CadParamKind values are stored in documents");
static_assert(CAD_PARAM_INTEGER == 3, "CadParamKind values are stored in documents");
static_assert(CAD_PARAM_BOOL == 4, "CadParamKind values are stored in documents");
static_assert(CAD_PARAM_TEXT == 5, "CadParamKind values are stored in documents");
static_assert(CAD_PARAM_OBJECT == 6, "CadParamKind values are stored in documents");
static_assert(CAD_PARAM_ELEMENT == 7, "CadParamKind values are stored in documents");
static_assert(CAD_PARAM_OBJECT_LIST == 8, "CadParamKind values are stored in documents");
static_assert(CAD_PARAM_ELEMENT_LIST == 9, "CadParamKind values are stored in documents");

// --- flags are a bitmask, and must stay one ----------------------------------------------

static_assert(CAD_PARAM_REQUIRED == 1u, "parameter flags are persisted");
static_assert(CAD_PARAM_COSMETIC == 2u, "parameter flags are persisted");
static_assert(CAD_PARAM_READ_ONLY == 4u, "parameter flags are persisted");
static_assert((CAD_PARAM_REQUIRED & CAD_PARAM_COSMETIC) == 0, "flags must not overlap");
static_assert((CAD_PARAM_REQUIRED & CAD_PARAM_READ_ONLY) == 0, "flags must not overlap");
static_assert((CAD_PARAM_COSMETIC & CAD_PARAM_READ_ONLY) == 0, "flags must not overlap");

}  // namespace

TEST_CASE("a descriptor reports the size it was compiled with", "[m2][abi]") {
    // The runtime half of the same rule. A caller sets struct_size from sizeof, and the value must
    // describe the struct the CALLER saw — which is what lets a host built years later distinguish
    // an old plugin's smaller struct from a corrupt one.
    CadParamDesc param{};
    param.struct_size = static_cast<uint32_t>(sizeof(CadParamDesc));
    param.struct_version = 1;

    CHECK(param.struct_size == sizeof(CadParamDesc));

    // A zeroed descriptor is what "the plugin forgot" looks like, and it must be distinguishable
    // from a legitimate one. It is, because no real descriptor can have size 0.
    const CadParamDesc forgotten{};
    CHECK(forgotten.struct_size == 0u);
    CHECK(forgotten.struct_size != sizeof(CadParamDesc));
}

TEST_CASE("a truncated descriptor is detectable, not merely smaller", "[m2][abi]") {
    // Simulates an OLD plugin: it reports the size of the struct as it existed when that plugin
    // was compiled, which is smaller than today's. The host must be able to tell how much of the
    // struct is real, using nothing but the reported size.
    const uint32_t oldSize = offsetof(CadParamDesc, default_value);

    CHECK(oldSize > 0u);
    CHECK(oldSize < sizeof(CadParamDesc));

    // The host's test for "does this caller have field X" is offset+size <= reported size. Under
    // the old size, `kind` is present and `default_value` is not — which is exactly the question
    // the negotiation has to answer, and it answers it with arithmetic rather than a version table.
    CHECK(offsetof(CadParamDesc, kind) + sizeof(uint32_t) <= oldSize);
    CHECK(offsetof(CadParamDesc, default_value) + sizeof(double) > oldSize);
}
