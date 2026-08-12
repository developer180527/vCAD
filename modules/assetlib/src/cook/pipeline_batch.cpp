// ── CookPipeline: batch cooks ────────────────────────────────────────────────
// cookAll and cookGraph — where a set of stale assets becomes a cost-weighted
// task graph with dependency edges, memory-budget admission, and a serialized
// drain lane. The per-record helpers these call (resolve, placeOutput,
// commitResult) are in pipeline.cpp.
//
// This is also where the dependency-hash snapshot is taken: ONE query for every
// asset's dependency hashes, because a query per asset inside a staleness loop is
// the mistake that was removed from the scanner.
// ── CookPipeline — cook ORCHESTRATION ────────────────────────────────────────
// What needs cooking, in what order, and what the registry records after.
// The mechanics live behind their own seams:
//   cook_key.h        identity + staleness (DDC keys)
//   cook_dispatch.h   execution mode (isolated child process / in-process)
//   ddc_manifest.h    cached-output record format (manifest of member blobs)
//   ddc.h             the content-addressed two-tier store
//   task_graph.h      cost-weighted DAG scheduler + thermal governance
#include "assetlib/cook_pipeline.h"
#include "assetlib/ddc_manifest.h"
#include "assetlib/task_graph.h"
#include "cook/dispatch.h"
#include "cook/key.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <unordered_map>

namespace assetlib {

// ── Batch cooks ──────────────────────────────────────────────────────────────

int CookPipeline::cookAll(std::function<void(int,int)> progress) {
    auto all   = m_registry.all();
    int  total = static_cast<int>(all.size());

    // One query for every asset's dependency hashes, not one per asset.
    const DepHashIndex depIdx = m_registry.allDependencySourceHashes();

    std::vector<UUID> stale;
    int backfilled = 0;
    for (auto& rec : all) {
        if (isStaleWith(rec, depIdx)) { stale.push_back(rec.uuid); continue; }
        if (backfillDdc(rec)) ++backfilled;         // warm .cache, cold DDC
        if (rec.state != AssetState::Ready
                && rec.state != AssetState::Failed) {   // fresh but unmarked
            auto r = m_registry.findByUUID(rec.uuid);
            if (r) { r->state = AssetState::Ready; m_registry.update(*r); }
        }
    }
    if (backfilled > 0)
        std::printf("[AssetLib] DDC: back-filled %d up-to-date asset(s)\n",
                    backfilled);

    std::atomic<int> done{ total - static_cast<int>(stale.size()) };
    int cooked = cookMany(stale, [&](const std::string&, bool) {
        if (progress) progress(done.fetch_add(1) + 1, total);
    });
    if (progress) progress(total, total);
    return cooked;
}

int CookPipeline::cookGraph(const std::vector<UUID>& uuids,
                            std::vector<ExtraTask> extras,
                            std::function<void(const std::string&, bool)> onResult,
                            std::function<bool()> shouldContinue) {
    struct Work {
        UUID                  uuid;
        Resolved              r;
        std::vector<UUID>     deps;
        std::vector<std::filesystem::path> outputs;
        CookResult            result;
    };

    // ── Phase 1 (caller thread): resolve records into self-contained work,
    // serving DDC hits inline — a hit is a hardlink + a registry row, there
    // is nothing to parallelize.
    std::vector<Work> work;
    work.reserve(uuids.size());
    const DepHashIndex depIdx = m_registry.allDependencySourceHashes();
    int hits = 0, backfilled = 0;
    for (const auto& uuid : uuids) {
        auto rec = m_registry.findByUUID(uuid);
        if (!rec) continue;
        if (!isStaleWith(*rec, depIdx)) {
            // Up to date. If the store somehow lacks it (wiped ~/.engine, a
            // tree copied from another machine), ingest it now rather than
            // paying a full cook at the next .cache wipe.
            if (backfillDdc(*rec)) ++backfilled;
            continue;
        }
        auto r = resolve(*rec, &depIdx);
        if (!r) continue;                   // no cooker / unreadable source

        if (ddcFetchRecord(m_ddc, r->key, r->outPath)) {
            commitResult(uuid, { .success=true }, r->key, r->cooker->version(),
                         r->outPath, {});
            ++hits;
            if (onResult) onResult(r->sourceRel, true);
            continue;
        }
        work.push_back(Work{ .uuid = uuid, .r = std::move(*r) });
    }
    const int numWork = static_cast<int>(work.size());
    if (hits > 0)
        std::printf("[AssetLib] DDC: %d asset(s) restored from cache\n", hits);
    if (backfilled > 0)
        std::printf("[AssetLib] DDC: back-filled %d up-to-date asset(s)\n",
                    backfilled);
    if (numWork == 0 && extras.empty()) return hits;

    // ── Phase 2: build the task graph ──────────────────────────────────────
    // Every miss is a node: work() = cook + DDC ingest (worker pool, memory-
    // governed, QoS-demoted — TaskGraph owns the thermal levers); done() =
    // registry commit + progress (drain lane = this thread, so the single
    // registry connection is never shared). Nodes are cost-weighted by the
    // cooker's estimate — the graph dispatches longest-first, so the 8K
    // texture starts at t=0 instead of straggling behind a hundred trinkets.
    // NOTE: `work` is fully sized above and must not reallocate now that
    // lambdas capture references into it.
    TaskGraph graph;
    int cooked = 0, cancelled = 0;
    const auto t0 = std::chrono::steady_clock::now();
    std::unordered_map<UUID, int> nodeByUuid;   // UUID hashes directly

    // Cancellation reaches the COOKS, not just the dispatcher: workers poll
    // this and SIGKILL their child, so quitting the editor doesn't wait out a
    // multi-minute bake. Must be thread-safe — CookService's reads an atomic.
    CancelFn isCancelled;
    if (shouldContinue)
        isCancelled = [&shouldContinue] { return !shouldContinue(); };

    for (auto& w : work) {
        CookContext estCtx;                 // estimate may peek the header
        estCtx.uuid       = w.uuid;
        estCtx.sourcePath = w.r.sourcePath;
        const size_t est  = w.r.cooker->estimatePeakBytes(estCtx);

        const int node = graph.add("asset:" + w.r.sourceRel, est,
            /*work — pool*/ [this, &w, &isCancelled] {
                CookContext ctx;
                ctx.uuid          = w.uuid;
                ctx.sourcePath    = w.r.sourcePath;
                ctx.outputPath    = w.r.tmpPath;   // never the final path
                ctx.addDependency = [&w](const UUID& dep) { w.deps.push_back(dep); };
                ctx.addOutput     = [&w](const std::filesystem::path& p) {
                    w.outputs.push_back(p);
                };
                w.result = dispatchCook(m_workerExe, *w.r.cooker, ctx, isCancelled);
                // DDC ingest on the pool too — hashing/copying the blobs of a
                // big mesh is real work the drain lane shouldn't serialize.
                placeOutput(w.result, w.r.key, w.r.tmpPath, w.r.outPath,
                            w.outputs);
            },
            /*done — drain*/ [this, &w, &cooked, &cancelled, &onResult] {
                // Cancelled: commit nothing, count nothing, report nothing —
                // the asset stays stale and cooks on the next pass. Reporting
                // it as a failure would just spam the shutdown log.
                if (w.result.cancelled) { ++cancelled; return; }
                if (!w.result.success && !w.result.skipped)
                    std::printf("[AssetLib] Cook FAILED: %s — %s\n",
                                w.r.sourceRel.c_str(), w.result.error.c_str());
                commitResult(w.uuid, w.result, w.r.key, w.r.cooker->version(),
                             w.r.outPath, w.deps);
                if (w.result.success) ++cooked;
                if (onResult)
                    onResult(w.r.sourceRel, w.result.success || w.result.skipped);
            });
        nodeByUuid.emplace(w.uuid, node);
    }

    // Dependency edges among the cook set (registry graph). Sparse today —
    // meshes cook their textures inline — but any cooker that READS another
    // asset's cooked output is ordered correctly from here on.
    for (auto& w : work) {
        const int self = nodeByUuid.at(w.uuid);
        for (const auto& dep : m_registry.dependencies(w.uuid)) {
            const auto dit = nodeByUuid.find(dep);
            if (dit != nodeByUuid.end() && dit->second != self)
                graph.addEdge(dit->second, self);
        }
    }

    // Extra tasks (scene cooks): run after their own referenced assets are
    // cooked AND committed — and immediately when none of them are cooking.
    std::vector<char> extraOk(extras.size(), 0);
    for (size_t i = 0; i < extras.size(); ++i) {
        ExtraTask& e  = extras[i];
        char&      ok = extraOk[i];
        const int node = graph.add("extra:" + e.name, e.estBytes,
            [&e, &ok] { ok = (e.run && e.run()) ? 1 : 0; },
            [&e, &ok] { if (e.onDone) e.onDone(ok != 0); });
        for (const auto& u : e.waitFor) {
            const auto dit = nodeByUuid.find(u);
            if (dit != nodeByUuid.end()) graph.addEdge(dit->second, node);
        }
    }

    TaskGraph::Options opts;
    opts.shouldContinue = shouldContinue;   // graph keeps its own copy
    graph.run(opts);

    const double ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
    std::printf("[AssetLib] Cooked %d/%d asset(s), %zu extra task(s) in %.1f ms "
                "(+%d from DDC)\n", cooked, numWork, extras.size(), ms, hits);
    if (cancelled > 0)
        std::printf("[AssetLib] %d cook(s) cancelled — left stale, will retry\n",
                    cancelled);
    return cooked + hits;
}

} // namespace assetlib
