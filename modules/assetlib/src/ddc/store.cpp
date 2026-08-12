// ── The two-tier content-addressed blob store ────────────────────────────────
// Roots, atomic ingest, hardlink materialization, and shared->local promotion.
// Every read/write of a blob passes through here; eviction lives in gc.cpp and
// key derivation in hash.cpp, so this file is only ever about MOVING BYTES
// safely between a cook's output, the store, and a project's .cache.
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

// ── Store ─────────────────────────────────────────────────────────────────────

fs::path DdcStore::defaultLocalRoot() {
    if (const char* v = std::getenv("ENGINE_DDC"); v && *v) return v;
#if defined(_WIN32)
    if (const char* v = std::getenv("LOCALAPPDATA"); v && *v)
        return fs::path(v) / "engine" / "ddc";
#endif
    if (const char* v = std::getenv("HOME"); v && *v)
        return fs::path(v) / ".engine" / "ddc";
    return fs::temp_directory_path() / "engine-ddc";
}

fs::path DdcStore::sharedRootFromEnv() {
    if (const char* v = std::getenv("ENGINE_DDC_SHARED"); v && *v) return v;
    return {};
}

DdcStore::DdcStore(fs::path localRoot, fs::path sharedRoot)
    : m_local(localRoot.empty() ? defaultLocalRoot() : std::move(localRoot))
    , m_shared(sharedRoot.empty() ? sharedRootFromEnv() : std::move(sharedRoot)) {
    std::error_code ec;
    fs::create_directories(m_local, ec);
    if (ec)
        std::fprintf(stderr, "[DDC] cannot create local store %s: %s\n",
                     m_local.string().c_str(), ec.message().c_str());
    // Deliberately do NOT create the shared root — if the mount is absent we
    // must degrade to local-only, not scribble a directory onto the mount
    // point and shadow the real cache when it comes back.
}

fs::path DdcStore::blobPath(const fs::path& root, const std::string& key) {
    // Fan out on the first byte so no directory collects millions of entries.
    return root / key.substr(0, 2) / (key + ".blob");
}


bool DdcStore::contains(const std::string& key) const {
    if (key.empty()) return false;
    std::error_code ec;
    if (fs::exists(blobPath(m_local, key), ec)) return true;
    return !m_shared.empty() && fs::exists(blobPath(m_shared, key), ec);
}

bool DdcStore::ingest(const fs::path& root, const std::string& key,
                      const fs::path& src) const {
    std::error_code ec;
    const fs::path blob = blobPath(root, key);
    if (fs::exists(blob, ec)) return true;     // first writer already won
    fs::create_directories(blob.parent_path(), ec);
    if (ec) return false;

    // Copy to a private temp name IN the destination directory, then rename:
    // rename is atomic on the same filesystem, so a concurrent reader can
    // never see a half-written blob — it sees nothing, or the whole thing.
    const fs::path tmp = uniqueTempPath(blob.parent_path(), key, "ingest");

    fs::copy_file(src, tmp, fs::copy_options::overwrite_existing, ec);
    if (ec) { fs::remove(tmp, ec); return false; }
    // Blobs are immutable — read-only so a stray ofstream (or a cooker handed
    // a hardlinked path by mistake) fails to open rather than corrupting the
    // cache for every project sharing it.
    makeReadOnly(tmp);
    fs::rename(tmp, blob, ec);
    if (ec) {
        removeBlob(tmp, ec);              // read-only by now: needs the helper
        std::error_code ec2;
        return fs::exists(blob, ec2);          // raced with another writer: fine
    }
    return true;
}

bool DdcStore::materialize(const fs::path& blob, const fs::path& dst) const {
    std::error_code ec;
    fs::create_directories(dst.parent_path(), ec);
    removeBlob(dst, ec);                        // replace, never write-through
    fs::create_hard_link(blob, dst, ec);        // zero-copy on same volume
    if (!ec) return true;
    ec.clear();
    fs::copy_file(blob, dst, fs::copy_options::overwrite_existing, ec);
    return !ec;
}

bool DdcStore::fetch(const std::string& key, const fs::path& dst) {
    if (key.empty()) return false;
    std::error_code ec;

    const fs::path local = blobPath(m_local, key);
    if (fs::exists(local, ec)) {
        if (materialize(local, dst)) { touchForLru(local); ++m_localHits; return true; }
        return false;
    }

    if (!m_shared.empty()) {
        const fs::path shared = blobPath(m_shared, key);
        if (fs::exists(shared, ec)) {
            // Promote into the local tier first, then materialize from local —
            // the next fetch of this key never touches the network again.
            if (ingest(m_local, key, shared) && materialize(local, dst)) {
                ++m_sharedHits;
                return true;
            }
            // Promotion failed (local disk full?) — serve straight from shared.
            if (materialize(shared, dst)) { ++m_sharedHits; return true; }
            return false;
        }
    }
    ++m_misses;
    return false;
}

bool DdcStore::store(const std::string& key, const fs::path& src) {
    if (key.empty()) return false;
    if (!ingest(m_local, key, src)) return false;
    ++m_stores;
    if (!m_shared.empty()) {
        std::error_code ec;
        if (fs::exists(m_shared, ec) && !ingest(m_shared, key, src))
            std::fprintf(stderr, "[DDC] shared push failed for %s (local copy "
                         "intact)\n", key.c_str());
    }
    return true;
}

bool DdcStore::storeBytes(const std::string& key, const std::string& bytes) {
    if (key.empty()) return false;
    // Spill to a private temp file, then reuse the file ingest path (same
    // atomicity, same tiering).
    std::error_code ec;
    fs::create_directories(m_local, ec);
    const fs::path tmp = uniqueTempPath(m_local, key, "bytes");
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f.write(bytes.data(), (std::streamsize)bytes.size())) {
            fs::remove(tmp, ec);
            return false;
        }
    }
    const bool ok = store(key, tmp);
    fs::remove(tmp, ec);
    return ok;
}

bool DdcStore::fetchBytes(const std::string& key, std::string& out) {
    if (key.empty()) return false;
    auto read = [&](const fs::path& blob) -> bool {
        std::ifstream f(blob, std::ios::binary);
        if (!f) return false;
        std::string s((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());
        if (f.bad()) return false;
        out = std::move(s);
        return true;
    };
    std::error_code ec;
    const fs::path local = blobPath(m_local, key);
    if (fs::exists(local, ec) && read(local)) {
        touchForLru(local); ++m_localHits; return true;
    }
    if (!m_shared.empty()) {
        const fs::path shared = blobPath(m_shared, key);
        if (fs::exists(shared, ec) && read(shared)) {
            ingest(m_local, key, shared);          // promote, best-effort
            ++m_sharedHits;
            return true;
        }
    }
    ++m_misses;
    return false;
}

void DdcStore::evictLocal(const std::string& key) {
    if (key.empty()) return;
    std::error_code ec;
    removeBlob(blobPath(m_local, key), ec);
}

DdcStore::Stats DdcStore::stats() const {
    return { m_localHits.load(), m_sharedHits.load(),
             m_misses.load(),    m_stores.load() };
}

} // namespace assetlib
