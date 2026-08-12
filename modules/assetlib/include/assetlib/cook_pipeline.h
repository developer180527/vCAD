#pragma once
#include "uuid.h"
#include "asset_registry.h"
#include <unordered_map>
#include "cooker.h"        // CookContext / CookResult / ICooker
#include "ddc.h"
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace assetlib {

// ── Cook pipeline ─────────────────────────────────────────────────────────────
// ORCHESTRATION: which assets need cooking, in what order, and what the
// registry records afterwards. The pieces it drives live behind their own
// seams — cook identity/staleness (`src/cook/key.h`), execution mode
// (`src/cook/dispatch.h`), the cached-output record format
// (`ddc_manifest.h`), the content-addressed store (`ddc.h`), and the
// cost-weighted scheduler (`task_graph.h`).
class CookPipeline {
public:
    explicit CookPipeline(AssetRegistry& registry,
                          std::filesystem::path projectRoot,
                          std::filesystem::path cacheRoot);

    void registerCooker(std::unique_ptr<ICooker> cooker);

    // Cook a single asset (by UUID). No-op if already up to date.
    CookResult cookOne(const UUID& uuid);

    // Cook all stale assets in the registry.
    // Returns number of assets cooked.
    // Progress callback receives (cooked, total).
    int        cookAll(std::function<void(int,int)> progress = {});

    // A non-asset job scheduled into the same cook graph (scene cooks).
    // run() executes on the worker pool AFTER every `waitFor` asset in the
    // cook set has been cooked AND committed; onDone(success) runs on the
    // caller thread. waitFor UUIDs not in the cook set are already fresh
    // (or DDC hits committed up front) and impose no edge.
    struct ExtraTask {
        std::string               name;                      // for logs
        size_t                    estBytes = (size_t)16 << 20;
        std::function<bool()>     run;
        std::vector<UUID>         waitFor;
        std::function<void(bool)> onDone;
    };

    // Cook a fixed UUID set as a cost-weighted task graph (TaskGraph):
    // DDC hits are served inline first; misses become graph nodes ordered
    // longest-first with dependency edges from the registry; extras (scene
    // cooks) run as soon as THEIR OWN assets land. Registry I/O stays on the
    // caller thread (the graph's drain lane); cook() and DDC ingest run on
    // the pool. onResult(sourcePath, success) is invoked serialized as each
    // asset finishes; shouldContinue() (optional) stops dispatching.
    // Returns assets cooked (including cache hits).
    int        cookGraph(const std::vector<UUID>& uuids,
                         std::vector<ExtraTask> extras = {},
                         std::function<void(const std::string&, bool)> onResult = {},
                         std::function<bool()> shouldContinue = {});
    // Compatibility wrapper: cookGraph with no extras.
    int        cookMany(const std::vector<UUID>& uuids,
                        std::function<void(const std::string&, bool)> onResult = {},
                        std::function<bool()> shouldContinue = {}) {
        return cookGraph(uuids, {}, std::move(onResult), std::move(shouldContinue));
    }

    // Force re-cook regardless of hash.
    CookResult forceRecook(const UUID& uuid);

    // Content-addressed staleness: an asset is stale iff the DDC key computed
    // from its CURRENT inputs (source hash ⊕ cooker id/version ⊕ settings ⊕ the
    // source hashes of its DEPENDENCIES) differs from the key of the last
    // attempt, or its materialized output vanished. No mtime comparison, no
    // global cook version — a cooker bump re-keys (and thus re-cooks) only that
    // cooker's assets, and a dependency edit re-keys only its dependents.
    bool         isStale(const AssetRecord& rec) const;
    bool         hasCookerFor(const std::string& ext) const;

    // The DDC key for this record's current inputs ("" when no cooker/hash).
    std::string  currentKey(const AssetRecord& rec, ICooker* cooker) const;

    // Snapshot every asset's dependency source hashes in ONE query. Callers
    // looping over many records should build this and pass it to isStaleWith,
    // rather than paying a query per asset.
    using DepHashIndexPublic =
        std::unordered_map<std::string, std::vector<std::string>>;
    DepHashIndexPublic dependencyHashIndex() const;
    bool  isStaleWith(const AssetRecord& rec,
                      const DepHashIndexPublic& idx) const;

    // Ingest an already-materialized, up-to-date output into the DDC when the
    // store has no record for its key. Closes the gap where a warm .cache sits
    // beside a cold DDC (a wiped ~/.engine, a project tree copied between
    // machines, a fresh CI container): staleness correctly says "not stale", so
    // no cook runs, so nothing was ever ingested — and the NEXT .cache wipe
    // then pays a full cook for outputs that were on disk the whole time. Such
    // a machine also never contributes to the shared tier.
    //
    // Cheap when the DDC is warm: one `contains()` (a stat) per fresh asset,
    // and real work only on an actual miss. Does NOT interfere with
    // forceRecook(), which clears ddcKey to make the record stale — stale
    // records take the cook path and never reach here.
    //
    // Returns true when a record was ingested.
    bool         backfillDdc(const AssetRecord& rec);

    DdcStore&       ddc()       { return m_ddc; }
    const DdcStore& ddc() const { return m_ddc; }

    // Out-of-process cooking: when set (and the binary exists), every cook
    // runs in a spawned `engine_cook_worker` child — a corrupt FBX that
    // SIGSEGVs Assimp kills one worker, not the editor, and each task gets a
    // HARD memory cap (child setrlimit) instead of the governor's estimate.
    // Unset/missing → cooks run in-process (COOK_INPROC=1 forces this).
    void setWorkerExecutable(std::filesystem::path exe) { m_workerExe = std::move(exe); }

private:
    ICooker*     findCooker(const std::string& ext) const;

    // Everything a cook of one record needs, resolved once on the caller
    // thread. Empty optional = not cookable (no cooker, or unreadable
    // source): the three cook entry points all made these same four
    // decisions before.
    struct Resolved {
        ICooker*              cooker = nullptr;
        std::string           key;         // DDC key of current inputs
        std::string           sourceRel;   // project-relative, for logs
        std::filesystem::path sourcePath;  // absolute
        std::filesystem::path outPath;     // .cache/<type>s/<uuid>.cooked
        std::filesystem::path tmpPath;     // cookers write HERE, never outPath
    };
    // asset uuid -> source hashes of its dependencies, snapshotted once per
    // batch. Threaded through resolve/isStale/currentKey as an optional pointer:
    // nullptr means "query this one record", which is right for the one-off
    // paths and wrong inside a loop over every asset.
    using DepHashIndex =
        std::unordered_map<std::string, std::vector<std::string>>;
    std::vector<std::string> depHashesFor(const AssetRecord& rec,
                                          const DepHashIndex* idx) const;

    std::optional<Resolved> resolve(const AssetRecord& rec,
                                    const DepHashIndex* idx = nullptr) const;

    // Move a finished cook's temp output into the DDC and materialize it at
    // outPath (falling back to a plain rename if the store is unusable), or
    // clean up the temp file on failure. Sets `res` to a failure if the
    // output cannot be placed at all. Runs on the caller thread OR a graph
    // worker — touches only the DDC and the filesystem, never the registry.
    void         placeOutput(CookResult& res, const std::string& key,
                             const std::filesystem::path& tmpPath,
                             const std::filesystem::path& outPath,
                             const std::vector<std::filesystem::path>& extras);

    // Shared body of cookOne/forceRecook. useFetch=false bypasses the DDC
    // read path (forceRecook must not re-fetch the very blob under suspicion).
    CookResult   cookInternal(const UUID& uuid, bool useFetch);

    // The ONE writer of a cook attempt's registry outcome (success, skip,
    // failure). Caller thread only — the graph's drain lane.
    void         commitResult(const UUID& uuid, const CookResult& res,
                              const std::string& key, uint32_t cookerVersion,
                              const std::filesystem::path& outPath,
                              const std::vector<UUID>& deps);

    AssetRegistry&              m_registry;
    std::filesystem::path       m_projectRoot;
    std::filesystem::path       m_cacheRoot;
    DdcStore                    m_ddc;      // roots from env (ENGINE_DDC[_SHARED])
    std::filesystem::path       m_workerExe;
    std::vector<std::unique_ptr<ICooker>> m_cookers;
};

} // namespace assetlib
