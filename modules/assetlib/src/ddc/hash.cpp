// ── Content hashing and cook-key composition ─────────────────────────────────
// BLAKE3 over bytes and files, and the ONE function that composes a cook key
// from its inputs. Separate from the store because it is pure: no filesystem, no
// tiers, nothing to configure. When you need to reason about what a key covers,
// this is the whole answer and it fits on a screen.
#include "assetlib/ddc.h"
#include "blake3.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>

#if defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN
    #define NOMINMAX
    #include <windows.h>
    #include <process.h>
#else
    #include <unistd.h>
    #include <sys/stat.h>
#endif

namespace assetlib {

namespace fs = std::filesystem;
// ── Hashing ───────────────────────────────────────────────────────────────────

static std::string hex(const uint8_t* d, size_t n) {
    static const char* k = "0123456789abcdef";
    std::string s(n * 2, '0');
    for (size_t i = 0; i < n; ++i) {
        s[i*2]   = k[d[i] >> 4];
        s[i*2+1] = k[d[i] & 0xf];
    }
    return s;
}

std::string blake3File(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return "";
    blake3_hasher h;
    blake3_hasher_init(&h);
    char buf[1 << 16];
    while (f.read(buf, sizeof(buf)) || f.gcount())
        blake3_hasher_update(&h, buf, (size_t)f.gcount());
    if (f.bad()) return "";
    uint8_t out[BLAKE3_OUT_LEN];
    blake3_hasher_finalize(&h, out, BLAKE3_OUT_LEN);
    return hex(out, BLAKE3_OUT_LEN);
}

std::string blake3Bytes(const void* data, size_t len) {
    blake3_hasher h;
    blake3_hasher_init(&h);
    blake3_hasher_update(&h, data, len);
    uint8_t out[BLAKE3_OUT_LEN];
    blake3_hasher_finalize(&h, out, BLAKE3_OUT_LEN);
    return hex(out, BLAKE3_OUT_LEN);
}

std::string computeDdcKey(const DdcKeyInputs& in) {
    // Canonical, unambiguous byte stream: length-prefixed fields so no
    // concatenation of two different input sets can collide ("ab"+"c" vs
    // "a"+"bc"). Bump the prefix if the key recipe itself ever changes.
    blake3_hasher h;
    blake3_hasher_init(&h);
    auto field = [&](const void* d, size_t n) {
        uint64_t len = (uint64_t)n;
        blake3_hasher_update(&h, &len, sizeof(len));
        blake3_hasher_update(&h, d, n);
    };
    auto str = [&](const std::string& s) { field(s.data(), s.size()); };

    str("engine-ddc-v1");
    str(in.cookerId);
    field(&in.cookerVersion, sizeof(in.cookerVersion));
    str(in.settings);
    str(in.sourceHash);
    auto deps = in.depHashes;                  // order-independent
    std::sort(deps.begin(), deps.end());
    for (const auto& d : deps) str(d);

    uint8_t out[BLAKE3_OUT_LEN];
    blake3_hasher_finalize(&h, out, BLAKE3_OUT_LEN);
    return hex(out, BLAKE3_OUT_LEN);
}

} // namespace assetlib
