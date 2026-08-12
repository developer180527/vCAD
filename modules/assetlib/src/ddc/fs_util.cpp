// ── Blob filesystem helpers ──────────────────────────────────────────────────
// Blobs are immutable and stored read-only, which makes ordinary file operations
// on them slightly unusual on every platform. Shared by store.cpp and gc.cpp —
// a second, subtly different uniqueTempPath is exactly how two writers end up on
// one file, so there is one definition and it is here.
#include "ddc/internal.h"
#include "blake3.h"

#include <algorithm>
#include <atomic>
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

namespace assetlib::ddcfs {

namespace fs = std::filesystem;

// Blobs are stored read-only (see store()). POSIX honours the containing
// directory's write permission when unlinking, so remove() just works — but
// Windows refuses to delete a FILE_ATTRIBUTE_READONLY file outright. Clearing
// the attribute first is what makes eviction and replacement portable;
// without it every blob removal on Windows fails silently and the cache grows
// without bound.
void removeBlob(const fs::path& p, std::error_code& ec) {
#if defined(_WIN32)
    const DWORD attrs = ::GetFileAttributesW(p.wstring().c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY))
        ::SetFileAttributesW(p.wstring().c_str(), attrs & ~FILE_ATTRIBUTE_READONLY);
#endif
    fs::remove(p, ec);
}

// Record "this blob was used just now" as its mtime, so GC's LRU order reflects
// USE and not ingest time. Best-effort and deliberately silent: the store stays
// correct if the touch fails (a read-only mount, a foreign owner), it just
// evicts in ingest order for that blob. Blobs are 0444, which is fine — setting
// mtime needs ownership, not write permission, and we own what we ingested.
void touchForLru(const fs::path& p) {
    std::error_code ec;
    fs::last_write_time(p, fs::file_time_type::clock::now(), ec);
}

// Mark a finished blob immutable.
void makeReadOnly(const fs::path& p) {
#if defined(_WIN32)
    ::SetFileAttributesW(p.wstring().c_str(), FILE_ATTRIBUTE_READONLY);
#else
    ::chmod(p.string().c_str(), 0444);
#endif
}
// A temp name unique across processes AND threads. Both must be in it: two
// cook workers on one machine share a pid-less name, and two graph workers
// storing the SAME key concurrently (duplicate assets, or two meshes with
// byte-identical embedded textures) share a thread-less name — either
// collision means both write the same file and one ships a truncated blob.
fs::path uniqueTempPath(const fs::path& dir, const std::string& key,
                        const char* tag) {
    static std::atomic<uint64_t> ctr{0};
#if defined(_WIN32)
    const uint64_t pid = (uint64_t)::_getpid();
#else
    const uint64_t pid = (uint64_t)::getpid();
#endif
    return dir / (key + "." + tag + "." + std::to_string(pid)
                      + "." + std::to_string(ctr.fetch_add(1)));
}

} // namespace assetlib::ddcfs
