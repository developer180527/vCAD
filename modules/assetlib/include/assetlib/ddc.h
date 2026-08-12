#pragma once
#include <filesystem>
#include <string>
#include <vector>
#include <cstdint>
#include <atomic>

namespace assetlib {

// ── Content hashing (BLAKE3-256, hex) ─────────────────────────────────────────
// THE hash for asset identity and DDC keys. Cryptographic strength is not
// paranoia here: on a SHARED cache a collision silently serves the wrong
// cooked bytes to every machine in the studio. (FNV-1a remains acceptable
// only for local change *detection*, never for content *addressing*.)
std::string blake3File(const std::filesystem::path& p);   // "" on I/O error
std::string blake3Bytes(const void* data, size_t len);

// ── DDC key ───────────────────────────────────────────────────────────────────
// The cooked output of an asset is a pure function of these inputs. Hash them
// and the hash names the output — that is the entire trick. Per-cooker id +
// version live in the key, so bumping the texture cooker never invalidates a
// single cooked mesh (the old global kCurrentCookVersion recooked the world).
struct DdcKeyInputs {
    std::string cookerId;                 // stable, e.g. "mesh", "texture"
    uint32_t    cookerVersion = 0;        // bump on output-format/logic change
    std::string settings;                 // cooker knobs that alter output
                                          // (quality tier, per-asset flags…)
    std::string sourceHash;               // blake3 of the source file bytes
    std::vector<std::string> depHashes;   // hashes of inputs BEYOND the source
                                          // file (sorted internally; reserved
                                          // for multi-file sources)
};
std::string computeDdcKey(const DdcKeyInputs& in);

// ── Derived Data Cache ────────────────────────────────────────────────────────
// Two-tier content-addressed blob store: a local tier (fast, per-machine,
// shared across every project on the box) and an optional shared tier (any
// path both machines can see — an NFS/SMB mount is a studio cache with zero
// server code). Read path: local → shared (hit promotes the blob into local).
// Write path: ingest local, then push shared best-effort — a dead network
// mount must never fail a cook that already produced correct output.
//
// Blobs are immutable and stored read-only; materialization into a project's
// .cache/ is by hardlink when possible (zero bytes copied), copy otherwise.
class DdcStore {
public:
    // Empty localRoot → defaultLocalRoot(). Empty sharedRoot → no shared tier.
    explicit DdcStore(std::filesystem::path localRoot  = {},
                      std::filesystem::path sharedRoot = {});

    // True if either tier holds the blob.
    bool contains(const std::string& key) const;

    // Materialize the blob for `key` at `dst` (replacing dst). False on miss.
    bool fetch(const std::string& key, const std::filesystem::path& dst);

    // Ingest a produced file under `key` (atomic: temp + rename; first writer
    // wins, identical content by construction). `src` is left in place.
    bool store(const std::string& key, const std::filesystem::path& src);

    // Small-record variants (manifests): store/fetch a byte string under a
    // key, same tiering and atomicity as file blobs.
    bool storeBytes(const std::string& key, const std::string& bytes);
    bool fetchBytes(const std::string& key, std::string& out);

    // Drop the LOCAL blob for `key` (force-recook path: suspicion of a bad
    // blob must bypass the cache, or the "re-cook" just re-fetches it).
    // Never touches the shared tier — other machines may be serving from it.
    void evictLocal(const std::string& key);

    const std::filesystem::path& localRoot()  const { return m_local; }
    const std::filesystem::path& sharedRoot() const { return m_shared; }

    struct Stats { uint64_t localHits=0, sharedHits=0, misses=0, stores=0; };
    Stats stats() const;

    // ── Garbage collection (LOCAL TIER ONLY) ────────────────────────────────
    // Without this the store grows forever: keys are derived from inputs, so
    // every source edit, every cooker version bump and every settings change
    // mints a NEW key and the old blob is never referenced again. Nothing else
    // collects them — CookService::collectGarbage reconciles a project's
    // .cache/ against its registry, which is a different store entirely.
    //
    // Eviction is LRU by mtime, which `fetch` touches on a local hit so the
    // order reflects USE rather than ingest time.
    //
    // A blob that is hardlinked into some project's .cache (link count > 1) is
    // skipped and counted as PINNED: deleting the store's link frees no bytes
    // at all (the inode survives via the project's link) and would only force a
    // re-ingest of something demonstrably in active use. Reporting those as
    // reclaimable would make `freedBytes` a lie.
    //
    // The SHARED tier is never collected here, for the same reason
    // `evictLocal` never touches it: a client cannot know what another machine
    // still needs, and one over-eager GC would silently cost every other
    // machine in the studio a full recook. Shared-tier retention is an
    // administrative decision on the box that hosts the mount.
    struct GcStats {
        uint64_t blobs           = 0;   // blobs examined
        uint64_t totalBytes      = 0;   // bytes resident in the local tier
        uint64_t pinnedBytes     = 0;   // hardlinked into a project (unreclaimable)
        uint64_t overBudgetBytes = 0;   // how far past maxBytes we started
        uint64_t deleted         = 0;   // blobs evicted (0 unless prune)
        uint64_t freedBytes      = 0;   // bytes actually reclaimed
    };
    // Evict least-recently-used blobs until the local tier fits `maxBytes`.
    // `prune == false` reports what WOULD be evicted and changes nothing —
    // matching `engine_cook --gc` vs `--gc-prune`.
    GcStats collectGarbage(uint64_t maxBytes, bool prune);

    // $ENGINE_DDC_MAX_MB (in MB), else kDefaultBudgetMb. 0 means unbounded, in
    // which case collectGarbage reports and evicts nothing.
    static constexpr uint64_t kDefaultBudgetMb = 20 * 1024;   // 20 GB
    static uint64_t budgetBytesFromEnv();

    // $ENGINE_DDC, else <home>/.engine/ddc — per-machine, cross-project.
    static std::filesystem::path defaultLocalRoot();
    // $ENGINE_DDC_SHARED, else empty (no shared tier).
    static std::filesystem::path sharedRootFromEnv();

private:
    static std::filesystem::path blobPath(const std::filesystem::path& root,
                                          const std::string& key);
    bool ingest(const std::filesystem::path& root, const std::string& key,
                const std::filesystem::path& src) const;
    bool materialize(const std::filesystem::path& blob,
                     const std::filesystem::path& dst) const;

    std::filesystem::path m_local;
    std::filesystem::path m_shared;
    mutable std::atomic<uint64_t> m_localHits{0}, m_sharedHits{0},
                                  m_misses{0},   m_stores{0};
};

} // namespace assetlib
