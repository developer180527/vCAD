// ── Garbage collection: budget + LRU ─────────────────────────────────────────
// Content-addressed blobs have no referrer to ask, so they cannot be reference
// counted: keys derive from inputs, and every source edit or cooker bump orphans
// a blob permanently. A budget is the only thing that can collect them. LOCAL
// TIER ONLY — see the header for why a client must never collect the shared one.
#include "assetlib/ddc.h"
#include "ddc/internal.h"
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
// Unqualified removeBlob/makeReadOnly/touchForLru/uniqueTempPath below
// resolve to ddc/fs_util.cpp — see ddc/internal.h.
using namespace ddcfs;

// ── Garbage collection ───────────────────────────────────────────────────────

uint64_t DdcStore::budgetBytesFromEnv() {
    if (const char* v = std::getenv("ENGINE_DDC_MAX_MB"); v && *v) {
        char* end = nullptr;
        const unsigned long long mb = std::strtoull(v, &end, 10);
        if (end != v) return (uint64_t)mb << 20;      // 0 is legal: unbounded
    }
    return kDefaultBudgetMb << 20;
}

DdcStore::GcStats DdcStore::collectGarbage(uint64_t maxBytes, bool prune) {
    GcStats st;
    if (m_local.empty()) return st;

    struct Entry {
        fs::path            path;
        uint64_t            bytes = 0;
        fs::file_time_type  used{};
    };
    std::vector<Entry> evictable;

    std::error_code ec;
    // Two levels: <root>/<2 hex>/<key>.blob. A non-recursive walk of the fan-out
    // dirs keeps this from wandering into anything else that shares the root.
    for (const auto& bucket : fs::directory_iterator(m_local, ec)) {
        if (!bucket.is_directory(ec)) continue;
        for (const auto& e : fs::directory_iterator(bucket.path(), ec)) {
            if (!e.is_regular_file(ec)) continue;
            if (e.path().extension() != ".blob") continue;   // skip *.ingest temps

            std::error_code sizeEc, linkEc, timeEc;
            const uint64_t bytes = (uint64_t)fs::file_size(e.path(), sizeEc);
            if (sizeEc) continue;
            ++st.blobs;
            st.totalBytes += bytes;

            // Hardlinked into a project's .cache: unlinking here reclaims
            // nothing, because the project's link keeps the inode alive.
            const uintmax_t links = fs::hard_link_count(e.path(), linkEc);
            if (!linkEc && links > 1) { st.pinnedBytes += bytes; continue; }

            const auto used = fs::last_write_time(e.path(), timeEc);
            evictable.push_back({ e.path(), bytes,
                                  timeEc ? fs::file_time_type{} : used });
        }
    }

    if (maxBytes == 0 || st.totalBytes <= maxBytes) return st;   // unbounded / fits
    st.overBudgetBytes = st.totalBytes - maxBytes;

    // Oldest use first.
    std::sort(evictable.begin(), evictable.end(),
              [](const Entry& a, const Entry& b) { return a.used < b.used; });

    // Evict until the TOTAL (pinned included — those bytes are really on the
    // disk) is under budget. If pinned data alone exceeds the budget we cannot
    // reach it; report honestly rather than deleting everything unpinned in a
    // futile attempt.
    uint64_t live = st.totalBytes;
    for (const Entry& e : evictable) {
        if (live <= maxBytes) break;
        if (prune) {
            std::error_code delEc;
            removeBlob(e.path, delEc);          // 0444: needs the helper
            if (delEc) continue;                // someone else got it, or in use
            ++st.deleted;
            st.freedBytes += e.bytes;
        }
        live -= e.bytes;
    }
    return st;
}

} // namespace assetlib
