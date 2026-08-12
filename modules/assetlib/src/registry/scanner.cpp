// ── Filesystem scan ──────────────────────────────────────────────────────────
// Walk the asset root, reconcile it with the registry, detect moves by content
// hash. The hot path of a warm editor start, and the only registry code that
// touches the filesystem — which is why the stat and hash helpers live here
// rather than in a shared header: nothing else needs them.
#include "assetlib/asset_registry.h"
#include "assetlib/ddc.h"
#include "registry/internal.h"

#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <chrono>
#include <utility>
#include <vector>

namespace assetlib {

// ── Content hash ──────────────────────────────────────────────────────────────
// BLAKE3-256 (64 hex chars) — the SAME hash the DDC keys cooked output by, so
// the registry's source_hash feeds straight into DDC key computation. The old
// FNV-1a 64-bit hash (16 hex chars) was fine for local change detection but
// is unsafe to content-address a shared cache with; scan() detects the short
// legacy format and rehashes in place.
static std::string hashFile(const std::filesystem::path& p) {
    return blake3File(p);
}
static bool isLegacyHash(const std::string& h) { return h.size() != 64; }

// ── Stat helpers ─────────────────────────────────────────────────────────────
// std::filesystem, NOT ::stat(p.string().c_str()). `path::string()` renders the
// NATIVE NARROW encoding, which on Windows is the active ANSI code page — so a
// source file whose name contains any non-ASCII character does not round-trip,
// ::stat fails, and statChanged reports "changed" forever. That fails safe (the
// asset recooks) but it recooks on EVERY scan, permanently, for any project with
// an accented or CJK filename. std::filesystem carries the wide path through on
// Windows and needs no encoding conversion at all.
//
// One representation note: file_time_type's epoch is unspecified before C++20's
// clock_cast, so the stored int64 is its raw tick count. That is fine here
// because the value is only ever compared against another value produced by
// this same function — it is a change detector, not a timestamp anyone reads.
// Consequence of the representation change: every record written by an older
// build has an incomparable mtime, so the first scan after this lands reports
// every asset as stat-changed and rehashes it once. That is safe — the hash
// then matches, so it takes the touch-only branch and nothing recooks — but it
// is one slow scan, once.
namespace {
// {mtime ticks, size}, or nullopt if the file cannot be examined at all.
//
// Read from the directory_entry the walk already produced. directory_entry
// CACHES the attributes it obtained while enumerating, so these are memory
// reads; the same two queries against a bare path are two fresh stat() calls,
// paid for every file on every scan even when nothing changed. On a 643-asset
// project that is ~1300 syscalls per warm start doing no useful work.
std::optional<std::pair<int64_t,int64_t>>
fileStamp(const std::filesystem::directory_entry& e) {
    std::error_code ec;
    const auto t = e.last_write_time(ec);
    if (ec) return std::nullopt;
    const auto sz = e.file_size(ec);
    if (ec) return std::nullopt;
    return std::make_pair((int64_t)t.time_since_epoch().count(), (int64_t)sz);
}
// Is this record's project-relative sourcePath under the root being scanned?
// Accepts either separator at the boundary: the stored path uses the platform's
// native one, and a hand-edited or migrated registry may hold the other.
bool underScannedRoot(const std::string& sourcePath, const std::string& prefix) {
    if (prefix.empty()) return true;
    if (sourcePath.size() <= prefix.size()) return false;
    if (sourcePath.compare(0, prefix.size(), prefix) != 0) return false;
    const char sep = sourcePath[prefix.size()];
    return sep == '/' || sep == '\\';
}

} // namespace

bool AssetRegistry::statChanged(const AssetRecord& rec,
                                const std::filesystem::directory_entry& e) {
    const auto s = fileStamp(e);
    if (!s) return true;                    // unreadable — assume changed
    return rec.sourceMtime != s->first || rec.sourceSize != s->second;
}

static void fillStat(AssetRecord& rec,
                     const std::filesystem::directory_entry& e) {
    if (const auto s = fileStamp(e)) {
        rec.sourceMtime = s->first;
        rec.sourceSize  = s->second;
    }
}


// ── Scanner ───────────────────────────────────────────────────────────────────
int AssetRegistry::scan(const std::filesystem::path& assetsRoot,
                        const std::filesystem::path& projectRoot) {
    // Timed because this is the warm-start hot path and the cost is invisible
    // otherwise: a scan that walks 600 files and cooks nothing still stats every
    // one of them, and nobody notices it doubling.
    const auto t0 = std::chrono::steady_clock::now();
    int scanned=0;
    int changed=0;
    std::unordered_set<std::string> seen;

    // Wrap all writes in one transaction — N individual writes become 1.
    // On a 10,000-asset project this is the difference between 10s and 100ms.
    // RAII: the scan walks the filesystem and can throw (or return early), and
    // an abandoned BEGIN leaves this connection stuck in an open transaction
    // for its whole lifetime — every later write fails and the DB looks
    // locked. The guard rolls back unless we reach the explicit commit.
    struct Transaction {
        sqlite3* db;
        bool     done = false;
        explicit Transaction(sqlite3* d) : db(d) {
            sqlite3_exec(db, "BEGIN;", nullptr, nullptr, nullptr);
        }
        void commit() {
            if (done) return;
            sqlite3_exec(db, "COMMIT;", nullptr, nullptr, nullptr);
            done = true;
        }
        ~Transaction() {
            if (!done) sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
        }
    } txn(db(m_db));

    // Move detection needs to look up records by content hash. Querying all()
    // per new file was O(new × total) full-table scans — a 10k-asset first
    // scan meant 10k SELECT *, i.e. 100M rows walked. Snapshot ONCE into a
    // hash index instead, built lazily so an up-to-date tree never pays for
    // it. `claimed` keeps one record from being re-pointed by two different
    // files: the old per-file all() re-read picked that up via the `seen`
    // check, a snapshot cannot.
    // The same snapshot also serves the existence check for EVERY walked file.
    // That used to be findBySourcePath(rel) per file, which is not merely N
    // queries — it builds the SQL with string concatenation and calls
    // sqlite3_prepare_v2 every time, so it is N statement COMPILATIONS, and
    // compilation is the expensive half. One all() replaces all of them.
    //
    // Safe as a snapshot because each `rel` is visited at most once by the
    // directory walk, so nothing looks up a record this scan already wrote.
    // Duplicate source_path rows (the index is not UNIQUE) keep the first, which
    // is what the old SELECT ... LIMIT-one behaviour did too.
    std::unordered_map<std::string, std::vector<AssetRecord>> byHash;
    std::unordered_map<std::string, AssetRecord>              bySourcePath;
    std::unordered_set<std::string> claimed;
    bool haveIndex = false;
    auto buildIndex = [&] {
        if (haveIndex) return;
        for (auto& rec : all()) {
            byHash[rec.sourceHash].push_back(rec);
            bySourcePath.emplace(rec.sourcePath, rec);
        }
        haveIndex = true;
    };
    auto hashIndex = [&]() -> decltype(byHash)& { buildIndex(); return byHash; };

    // error_code overload: an unreadable directory mid-walk must not throw
    // past the transaction guard (it would still roll back, but a scan that
    // reports what it managed to do beats one that unwinds into the caller).
    // Normalised ONCE, because the relative path below is computed lexically
    // against it and a non-normal root would produce a different string than
    // fs::relative did — which the registry would read as every asset moving.
    const std::filesystem::path rootNorm = projectRoot.lexically_normal();

    // Project-relative prefix of the root being scanned, in exactly the form
    // sourcePath is stored in (same lexically_relative + .string()), so the
    // comparison below cannot disagree with what the walk wrote. Empty or "."
    // means the scanned root IS the project root, so every record is in scope.
    std::string scanPrefix =
        assetsRoot.lexically_normal().lexically_relative(rootNorm).string();
    if (scanPrefix == ".") scanPrefix.clear();

    std::error_code walkEc;
    for (auto& entry : std::filesystem::recursive_directory_iterator(
             assetsRoot, std::filesystem::directory_options::skip_permission_denied,
             walkEc)) {
        if (!entry.is_regular_file()) continue;
        auto ext=entry.path().extension().string();
        for (auto& c:ext) c=static_cast<char>(std::tolower(c));
        if (assetTypeFromExtension(ext)==AssetType::Unknown) continue;

        ++scanned;
        // lexically_relative, NOT fs::relative. fs::relative runs
        // weakly_canonical on both operands first, which stats every component
        // of every path — filesystem work, per file, to answer a question that is
        // pure string manipulation once the root is normalised. The walked paths
        // are all built by the iterator from that same root, so they are already
        // in normal form and the lexical answer is identical.
        auto rel=entry.path().lexically_relative(rootNorm).string();
        seen.insert(rel);
        buildIndex();
        std::optional<AssetRecord> existing;
        if (auto it = bySourcePath.find(rel); it != bySourcePath.end())
            existing = it->second;

        if (existing) {
            // The file is here, so a Missing state is wrong — either it came
            // back, or an older build's unscoped sweep mis-marked it (see the
            // sweep below). Clear it and let cookAll promote to Ready once it
            // confirms the output is current; state is not what decides
            // staleness, so this cannot trigger a spurious recook.
            if (existing->state == AssetState::Missing) {
                existing->state = AssetState::Registered;
                fillStat(*existing, entry);
                update(*existing);
                ++changed;
                continue;
            }
            // Fast skip only when the stat is unchanged AND the stored hash is
            // already BLAKE3 — legacy FNV hashes are upgraded in place once.
            if (!statChanged(*existing, entry)
                    && !isLegacyHash(existing->sourceHash)) continue;
            auto newHash=hashFile(entry.path());
            if (newHash==existing->sourceHash) {
                fillStat(*existing,entry); update(*existing);        // touch only
            } else {
                existing->sourceHash=newHash;
                existing->state=AssetState::Stale;
                fillStat(*existing,entry);
                update(*existing);
                std::printf("[AssetLib] Stale: %s\n",rel.c_str());
                ++changed;
            }
        } else {
            auto newHash=hashFile(entry.path());

            // Moved/renamed file? Same content hash as a record whose source
            // file is gone — re-point that record instead of minting a new
            // UUID, so scene references (which key on UUID) survive renames.
            std::optional<AssetRecord> moved;
            auto& index = hashIndex();
            if (auto it = index.find(newHash); it != index.end()) {
                for (auto& rec : it->second) {
                    if (rec.sourcePath==rel) continue;
                    if (claimed.count(rec.uuid.toString())) continue;  // taken
                    if (seen.count(rec.sourcePath)) continue;          // still present
                    if (std::filesystem::exists(projectRoot/rec.sourcePath)) continue;
                    moved=rec; break;
                }
            }
            if (moved) {
                claimed.insert(moved->uuid.toString());
                std::printf("[AssetLib] Moved: %s -> %s (%s)\n",
                            moved->sourcePath.c_str(),rel.c_str(),
                            moved->uuid.toString().c_str());
                moved->sourcePath=rel;
                // Content is identical, so an existing cooked binary is still
                // valid; only revive records previously marked Missing.
                if (moved->state==AssetState::Missing)
                    moved->state=moved->cookedPath.empty()
                        ? AssetState::Registered : AssetState::Ready;
                fillStat(*moved,entry);
                update(*moved);
                ++changed;
                continue;
            }

            AssetRecord rec;
            rec.uuid=UUID::generate(); rec.type=assetTypeFromExtension(ext);
            rec.state=AssetState::Registered; rec.sourcePath=rel;
            rec.sourceHash=newHash;
            fillStat(rec,entry);
            insert(rec);
            std::printf("[AssetLib] New: %s -> %s\n",rel.c_str(),rec.uuid.toString().c_str());
            ++changed;
        }
    }
    // Mark deleted files as Missing. Deliberately a FRESH all(), not the
    // snapshot above: move detection re-points a record's sourcePath during the
    // walk, and the snapshot still holds that record under its OLD path. Reading
    // the snapshot here would find the old path absent from `seen` and mark the
    // asset Missing — immediately after correctly re-pointing it. The extra query
    // buys freshness, which this loop cannot do without.
    //
    // SCOPED TO THE ROOT JUST SCANNED. One registry holds records from more than
    // one root: CookService scans the project's assets AND the engine's own
    // defaults against the same projectRoot, and the runtime scans only the
    // project's. An unscoped sweep therefore had every scan declaring the OTHER
    // root's assets deleted — the project scan marked every engine shader
    // Missing, the engine scan marked every project asset Missing, and a plain
    // editor boot marked all the engine defaults Missing every time. A scan of
    // root X can only make claims about assets under X.
    for (auto& rec:all()) {
        if (rec.state==AssetState::Missing) continue;
        if (!underScannedRoot(rec.sourcePath, scanPrefix)) continue;
        if (seen.count(rec.sourcePath)) continue;
        setState(rec.uuid,AssetState::Missing);
        std::printf("[AssetLib] Missing: %s\n",rec.sourcePath.c_str());
        ++changed;
    }
    txn.commit();
    if (walkEc)
        std::printf("[AssetLib] scan: %s (partial scan committed)\n",
                    walkEc.message().c_str());
    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - t0).count();
    std::printf("[AssetLib] scan: %d asset(s) in %.1f ms (%d changed)\n",
                scanned, ms, changed);
    return changed;
}

} // namespace assetlib
